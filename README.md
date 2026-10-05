# Minecraft X Saints Row: The Third Remastered

Real Minecraft running inside Saints Row: The Third Remastered. Steve walks Stilwater where the Boss walks, you
build with Minecraft blocks on Stilwater's streets, Minecraft TNT blows up as Saints Row explosions, and
Minecraft swords and arrows hurt Saints Row's people.

> **Status: early.** Built and partly verified in the real game; see `beads/tasks.md` for what is verified and
> what isn't yet. Back up your saves (`Steam\userdata\<id>\978300\remote`). Fan project; you need both games.

## How it works

Both games run at once. Saints Row is in charge of the world, vehicles and the camera; on foot in Minecraft
mode, Minecraft moves the player and the Boss follows. Minecraft draws:

```
Saints Row (SRTTR.exe)                                    Minecraft 26.3 + Fabric (mc/)
  link/ = dinput8.dll                                       dev.mcsr3 (from the Minecraft x GTA V example)
    camera + player, people, ground -- WebSocket 25599 -->    HostLink: renders from SR3's camera
                                    <-------------------      explosions, hits on SR3's people
    compositor (Present hook)  <-- shared memory ---------    frames: world colour + depth, HUD/hand
```

- **Camera:** SR3's game camera is read every frame and Minecraft renders from exactly that camera, at SR3's
  window size. SR3 is metres, Y up, left-handed; Minecraft gets `(x, y, -z)`.
- **Drawing:** at SR3's Present, Minecraft's world layer is depth-tested against SR3's depth buffer (its
  projection is read from SR3's constant buffers), then Minecraft's hand and HUD go on top.
- **Ground:** raycasts around the player (inside SR3's update) find SR3's floors and walls; they become invisible
  blocks in Minecraft (barrier columns topped with an invisible layer in eighths of a block, so steps match), and
  Minecraft walks you, its mobs and its blocks on them.
- **Fighting:** SR3's people nearby get invisible stand-ins in Minecraft. Steve's sword and arrows (and mobs) hit
  those, and the damage is applied to the real person. Minecraft explosions become SR3 explosions.
- **Lua:** the link can run Lua in SR3's gameplay scripts (used for the vehicle check, and `tools/srlua.py`).

Reverse-engineering notes: `docs/RE.md`.

## Requirements

- Windows 10/11, Saints Row: The Third Remastered on Steam (folder in `config.yaml`).
- Visual Studio 2022 Build Tools with "Desktop development with C++", and CMake.
- Python 3.10+ with `pip install pyyaml numpy pillow websockets lz4`.
- A JDK 25 in `tools/jdk-25*` (portable Temurin: `tools/setup.sh` downloads one).
- SR3 windowed or fullscreen: Minecraft renders at SR3's window size and follows it when it changes.

## Setup and play

```bash
bash tools/setup.sh           # JDK 25 into tools/
python build.py --install     # builds link/ and copies dinput8.dll next to SRTTR.exe
python play.py                # starts Minecraft (dev client), then Saints Row
```

- The first Minecraft start downloads Minecraft (a few minutes). It opens a void creative world by itself.
- Load your save in Saints Row. Minecraft links up by itself; its hotbar shows at the bottom.
- To remove the SR3 side: `python build.py --uninstall` (deletes `dinput8.dll`; the `mcsr3\` log folder stays).

### Controls

**F7** switches between the two modes (in a vehicle it's always Saints Row mode).

**Minecraft mode** (on foot, the default): first person, Minecraft's hand, hotbar and hearts. Minecraft moves
you: its walking speed, jumping, sprinting, sneaking and creative flight (double-tap Space). The Boss follows;
SR3 keeps him on the ground or a roof under you while you fly. His guns, SR3's weapon wheel, weapon dial (with
the health ring) and crosshair are off; SR3's minimap, money, respect and objectives stay.

| Key | Action |
|---|---|
| WASD, Space, Shift, Ctrl | Minecraft: walk, jump / fly, sneak, sprint |
| Mouse | Look (all the way up and down, like Minecraft) |
| Esc | Saints Row's pause menu (or closes an open Minecraft screen) |
| Left / right mouse, wheel, 1-9 | Minecraft: break/attack, place/use, hotbar |
| E, Q, F, T, / ... | Minecraft: inventory, drop, swap hands, chat, commands |
| O | Minecraft's menu |

While a Minecraft screen is open (inventory, chat, menu) every key and click goes to Minecraft and the mouse
moves a cursor. To use Saints Row's own actions (cars, doors, missions, the crib menu), switch to Saints Row
mode with F7.

**Saints Row mode**: plain Saints Row in third person, every control is Saints Row's. Blocks and mobs stay
in the world; Steve and Minecraft's HUD are hidden.

## Dev tools

| Tool | What |
|---|---|
| `tools/devloop.py` | Restart SR3 with a fresh build and load the latest save (waits until nobody's used the PC for 2 minutes) |
| `tools/srlua.py "mclog(get_object_pos(LOCAL_PLAYER))"` | Run Lua in SR3's gameplay state |
| `tools/mccmd.py "give @a stone 64"` | Send a command (or any host message) to Minecraft |
| `tools/mcframe.py tmp/f` | Save Minecraft's latest exported frame (world, depth, overlay) |
| `tools/memscan.py` | Out-of-process memory search in SR3 |
| `tools/vpp.py` | Read SR3's `.vpp_pc` archives (read only) |
| `<game>\mcsr3\dumpdepth`, `scanproj` | Touch to dump SR3's depth buffer / log every projection it uploads |

## Known issues

- SR3's HUD (minimap, money, weapon dial) can disappear in both modes; under investigation.
- Sniper scopes and aiming zoom SR3's view but not Minecraft's: blocks look off while zoomed.
- SR3's cars brake at Minecraft blocks rather than crash into them.
- Furniture Minecraft doesn't know about can snap Steve back to the Boss.
- Built against one game build (offsets in `docs/RE.md`): a Saints Row update can break the link (it logs what
  it couldn't find and switches that feature off).

Full list: `beads/tasks.md`.

## Credits and licences

MIT licence (`LICENSE`). Third-party code and licences: `THIRD_PARTY_NOTICES.md`.

- The Minecraft mod (`mc/`), the WebSocket client and the frame reader come from
  [universal-modder](https://github.com/rehan-remade/universal-modder)'s Minecraft x GTA V example (MIT).
- [SkyCraft](https://github.com/chasmlol/SkyCraft) by chasmlol (MIT): the idea and its Half-Life port before this.
- Lua signatures from [SaintExec](https://github.com/Nathnefo/SaintExec) (GPL-3.0; patterns only, no code);
  packfile format from [ThomasJepp.SaintsRow](https://github.com/saintsrowmods2/ThomasJepp.SaintsRow); SR3 script
  docs from Kinzie's Toy Box (saintsrowmods.com).
- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2), downloaded at build time.
- [Fabric](https://fabricmc.net/) for the Minecraft side.
- Made by [SawyerTheNerd](https://github.com/SawyerTheNerd); written with Claude Code (Anthropic), including
  the reverse engineering in `docs/RE.md`.
- Saints Row belongs to Deep Silver / Volition, Minecraft to Mojang Studios and Microsoft. Neither game is
  included; you need your own copies.
