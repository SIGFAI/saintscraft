# Saints Row: The Third Remastered: reverse-engineering notes

Everything the link (`link/`) relies on, how it was found, and how it is checked at runtime.

**Build:** Steam, app 978300, `SRTTR.exe` 21,474,968 bytes (dated 2024-08). Offsets are relative to `SRTTR.exe`'s
base (image base `0x140000000` in a disassembler). Where a fixed offset is used, the link compares the function's
first bytes first and turns that feature off (logged) on a mismatch.

**Tools:**
- static: [Vibe-Reverse-Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering) `retools` (disasm,
  decompiler, xrefs, string search);
- live: the link's own dev channel (`tools/srlua.py`: Lua in the game; `mc_read`, `mc_obj`, `mc_cfunc`,
  `mc_hwwatch`), `tools/memscan.py` (out of process) and the minidumps in `<game>\dumps`.

## Loading

- `SRTTR.exe` imports `DirectInput8Create` from `dinput8.dll`, so a `dinput8.dll` in the game folder loads first.
- No anti-cheat. The game is DirectX 11 (`D3D11CreateDevice`, `CreateDXGIFactory`).

## Lua 5.1 (gameplay scripts)

- Statically linked Lua 5.1.4, `lua_Number` = double (`lua_pushnumber` stores with `movsd`). `TValue` is 16 bytes.
  `lua_State`: `top` +0x10, `base` +0x18, `l_G` +0x20, `status` +0x0A.
- Signatures from [SaintExec](https://github.com/Nathnefo/SaintExec) (SRIV Re-Elected, the same CTG engine) match
  SRTTR uniquely: `luaL_loadbuffer` 0xa156e0, `lua_pcall` 0xa11a00, `lua_setfield` 0xa12090, `lua_tolstring`
  0xa122d0, `lua_pushcclosure` 0xa11ab0, `lua_get_current_thread` 0x836490. Ours: `lua_gettop` 0xa11740
  (`48 8B 41 10 48 2B 41 18 48 C1 F8 04 C3`), `lua_resume` 0xa13650.
- The gameplay state is created as `"game play"` (`lea rcx, "game play"; call create; mov [rip+slot], rax`); the
  slot is 0x1fca220. The interface state is a separate universe.
- Gameplay Lua runs on the main thread, the one pumping messages (`PeekMessageA`). In free roam the gameplay
  scripts are mostly idle, so the link runs its jobs from the top of the message pump.
- The globals table is locked after setup; `_PrepareForDynamicGlobals('')` unlocks it. There is no `string`
  library.
- The script actions are listed in Kinzie's Toy Box (SR3 SDK docs, saintsrowmods.com); `mc_cfunc(f)` gives the C
  function behind one.

## Objects, the player, humans

| What | Where | How found |
|---|---|---|
| object by script name | 0x638c00 `(name, 0)` | `get_object_pos` 0x645a60 → 0x63f130 |
| human by script name | 0x629ae0 `(name, 0)`; the player is `"#PLAYER1#"` | `teleport` 0x65dfc0 |
| position | object +0x60 (3 floats); 3x3 orientation at +0x6c (rows right, up, forward) | `get_object_pos` |
| object type | +0x4c, flags in the table at 0x28e4760 (+0xe bit 0 = human); +0x4b bits 0x10/4 = gone | `get_num_humans_in_trigger` 0x645880 |
| objects in a box | 0x32bfe0 `(float4* min, float4* max, u64* handles, int max, 0x2f, 0, 0)` | same |
| handle → object | 0x48910 `(0x117f458, u64* handle, 0x28e4748)` | same |
| damage a human | 0x4fadc0 `(human, attacker, float hp, 0, 0, u32* flags)`, flags 0x100 (+0x200: no script callbacks) | `character_damage` 0x63aa20 |

- **Position writes don't stick.** Each frame the human's Havok body (`human+0xe30 → +0x20 → [+0x90] → +0x30`,
  plus a world offset from `+0x30`) is copied into +0x60 (`0x4f9b20` → vtable `+0x88` → 0x4db540 → 0x5f2c40 →
  `object_set_pos` 0x4b4b60). Found with a hardware write watchpoint on +0x60 while walking.
- **Internal teleport** `human_teleport` 0x522eb0 `(human, pos, orient, 1, 1, 0, 0x2ab610(human, 4), resetCam,
  check)` crashed when called from the dev channel (inside Lua it reaches script callbacks); not called directly.
  `mc_tp` (luadev) still crashes it from a Lua job (2026-10-04).
- **Moving the player: write his Havok body.** The translation at `human+0xe30 → +0x20 → +0x90 → +0x30`, `+0x30`
  (3 floats; what 0x378670 → 0xb325e0 reads each frame) moves him next frame with no fade; it holds with
  `human_gravity_enable(name, false)`. This is how Minecraft mode makes the Boss follow Steve every tick.
- **Not for every tick: Lua `teleport_to_object(name, target, bool, bool, dx, dy, dz, heading)`** (0x65e4b0). For the
  player it goes through 0x578a00: the request is queued (0x39f370, two 64-byte slots), the stream centre moves
  (0x437220), and game state 8 is pushed (0x2e1380) — a **fade to black and back of about a second** while the
  destination streams in. Skipping the state push stops the move too. It lands him on the nearest surface he can
  stand on (< ~2 m up snaps back to the floor).
- **Hiding the player from rendering only:** `+0x53` bit 0 (`character_hide`'s flag) is read by the renderer,
  but it also stops the character's update; set it after the frame's update (the camera update hook) and clear
  it at the top of the next frame (first `PeekMessage`).
- **Object table** (all objects): open-addressing hash at 0x117f458, capacity u32 at +0x8, slots at +0x10, empty
  slot = `*0xe473f0`. Walked directly for nearby humans; the box query 0x32bfe0 crashed outside the game's update.
- **Weapons (Lua):** `inv_weapon_disable_all_slots(bool)` leaves the player unarmed (fists, no weapon dial);
  `hud_inventory_disable(bool)` turns the weapon wheel off. Both undo with `false`.

## Camera

- The game camera is a static struct at 0x13ada40: position +0x60, rotation rows +0x80/+0x90/+0xa0 (right, up,
  forward; 16-byte stride), a copy at +0xb0, FOV at +0xe0. Found by scanning for float triples 1-8 m from the
  player that moved when the mouse orbited the camera.
- The camera update 0xe2070 (main thread, inside the game's update) copies the computed camera (+0x70, +0xb0)
  into the render camera (+0x60, +0x80). Hooked: after it, the render camera can be moved (first person) and
  raycasts are safe. The orbit camera's pitch stops short of looking at the floor; first person keeps its own.
- The FOV (50) is **horizontal at a 4:3 aspect** (Hor+): the scene projection in the constant buffers has
  `ys = 2.85945 = 1 / (tan(25°) · 3/4)`, i.e. a vertical FOV of 38.55° at any aspect.
- Axes: x right, y up, z forward, left-handed (right × up = forward). Metres. Minecraft gets `(x, y, -z)`.

## Rendering

- Back buffer `R8G8B8A8_UNORM_SRGB` (format 29) at the window size.
- Scene depth: the back-buffer-sized `R24G8_TYPELESS` (D24S8) target the frame binds most (`OMSetRenderTargets`),
  shader-readable. Standard Z: `z = a + b / t`, with the scene projection measured live from the constant buffers
  (`near 0.15, far ~8171` in gameplay: `a = 1.000018, b = -0.150003`). The main menu uses another projection
  (60°, near 0.1).

## Explosions

- `explosion_create` 0x642c10 → type by name 0x11b330 `(const char*)` → spawn 0x118db0 `(info, source, owner,
  identity 0x11ac380, float* pos, 0, 1, 0, 0, 0, 0, 0, 1.0f, 0, u16* zone)`; the zone word is read from an object
  near the spot (+0x92).
- Type names come from `explosions.xtbl` in `misc_tables.vpp_pc` (100 types): `Grenade` (radius 4), `Satchel`
  (9), `RPG` (5), `Car Bomb` (30), ...

## Packfiles (.vpp_pc)

- Version 6, but the Remaster widened it: header fields from 0x150 and the 48-byte directory entries are u64,
  sections are 4 KB aligned (directory at 0x1000), and compressed files are a 16-byte header (magic, ?,
  compressed size, size) plus an LZ4 block. `tools/vpp.py` reads them.

## Input

- Mouse: raw input (`GetRawInputData`, `RIM_TYPEMOUSE`). Keyboard: window messages (`WM_KEYDOWN`/`UP`, through
  `PeekMessageA`) plus `GetKeyboardState` (which also reports mouse buttons). DirectInput devices exist but
  report nothing. SR3 imports no `GetAsyncKeyState`/`GetKeyState`.
- Pause: a counter at 0x295b624 (incremented by 0x83cb30, decremented by 0x83cb50); > 0 on the pause menu, the
  map and the phone. Found by diffing statics across pause/unpause cycles.

## Not found (yet)

- SR3's interface (vint) Lua state, to hide single HUD elements (health) while keeping the minimap.

## Ray casts

- `havokRaycast` 0x32ede0 `(query*, results*, true)`, synchronous; only safe inside the game's update (the camera
  update hook). Query (0x80 bytes, 16-aligned): +0x00 from, +0x10 to (float4), +0x60 ignore list, +0x68 filter
  callback `bool (handle, user, x)` (handle 0 = static world), +0x70 collision filter 0x50, +0x74 mode (0
  closest), +0x78 layer. Results `{data, count, capacity = 0x80000000}`; hits are 0x60 bytes: position +0x00,
  normal +0x20, fraction +0x30. Free with `0x976870(0x976e40(), data, 0x50, capacity & 0x3fffffff)` when the
  capacity's top bit is clear.
