# Tasks for Minecraft X SR3

The one task list (replaces TASKMANAGER.md). "Verified" = seen working in the real game.
The user OK'd closing/restarting SR3 + Minecraft whenever needed (2026-10-04); still say what was done.

## Verified in game
- [x] Minecraft drawn into SR3 from SR3's camera, depth-tested, re-projected; hidden on SR3's pause menu, map and loading screens (pause counter)
- [x] F7 modes. Minecraft mode: first person in both games (Boss hidden from rendering only), Minecraft hand/HUD/hotbar. Saints Row mode: third person, no Steve, no Minecraft HUD/held items, built blocks stay
- [x] Minecraft screens (inventory, menu): every key/click to Minecraft, drawn cursor; SR3's own key state hidden (no crib menu on E)
- [x] Right click places blocks / uses items (Minecraft 26 numbers buttons like SDL: 1 L, 2 M, 3 R; passed through unchanged) (2026-10-04)
- [x] Creeper spawn eggs spawn creepers; creeper explosions become SR3 grenade explosions (link log) (2026-10-04)
- [x] Steve's sword hits SR3 people ("player hit SR3 person 2 for 20.0 (ok)") (2026-10-04)
- [x] First person looks all the way down/up (own pitch +-89 deg; SR3's orbit camera stopped short of the floor) (2026-10-04)
- [x] Minecraft mode: no SR3 guns (weapon slots + weapon wheel disabled, restored in SR3 mode); clicks hidden from SR3 key state + button messages (2026-10-04)
- [x] Minecraft movement: walk, half-block steps, jump, creative flight up/down; the Boss follows (teleport after Minecraft's player every tick) (2026-10-04)
- [x] Typing in Minecraft's chat and search (WM_CHAR made before the key-down is swallowed) (2026-10-04)
- [x] Steve's arrows hit SR3 people (the GTA "fly through proxies" mixin is gone); hits knock them over (character_ragdoll's internals); fire/lava/flame arrows/fire aspect set them on fire (character_ignite's internals); mobs leave dead people alone (health +0x1ea8) (2026-10-04, log)
- [x] Long ender pearl throws land on SR3's world (pearls traced through SR3's raycasts); arrows stick in SR3 walls (2026-10-04, log)
- [x] SR3 cars no longer leave invisible blocks in the road (vehicles skipped by the ground rays); cars stop at player-built blocks (vehicle_stop's internals) (2026-10-04, log)
- [x] Saints Row mode gunfire sets off TNT and hits mobs; crossbow fireworks burst as SR3 explosions (2026-10-04, TNT only)
- [x] Fullscreen: Minecraft follows SR3's window size; a Minecraft screen open before connecting isn't missed (2026-10-04)
- [x] No black "loading screen" while moving: the Boss follows by writing his Havok body position each tick (teleport_to_object faded to black ~1 s every move); camera stays out of walls (raycast from the Boss's head) and in front of his holstered guns (2026-10-04)
- [x] Ground: raycast columns topped with an invisible layer block (eighths); levelling clears all old host ground within 48 blocks (2026-10-04)
- [x] No selection outline on the invisible ground

- [x] Ceilings near the player (upward rays, 6 blocks): flying indoors stops under the ceiling (2026-10-04)
- [x] Walls: chest-height rays become 3-block barrier columns; rays passing through remove them (2026-10-04)

## Architecture (report: %TEMP%\architecture-review-20261004-0135.html; waiting for the user to pick)
- [ ] Cut the GTA leftovers (Nether, projectile tracing, spawn director: ~700 lines); MobWar -> ped proxies only
- [ ] One home for the wire protocol (both sides from one message list)
- [ ] One mode state sent explicitly; coordinates as a pure module; ground model behind a probe seam (testable)

## Bugs / limits
- [ ] SR3's HUD hidden in both modes since ~12:30 on 2026-10-04, through restarts; a HUD_ALL_ELEM visible display state brings it back (cause unknown; creating one at save load didn't help)
- [ ] Sniper scope/aim zoom: Minecraft doesn't zoom with SR3 (attempts reverted: tmp/before-scope-revert.patch); SR3's HUD drawn under Minecraft (a HUD-target mask worked for the minimap, reverted with it)
- [ ] Cars only brake at blocks (no crash); the player's own car untested
- [ ] Furniture: SR3's physics pushes the Boss out of props Minecraft walks through; the 0.8 m / 0.4 s resync snaps Steve back (re-check now that the Boss is placed directly)
- [ ] Vehicles: verify SR3 gets all input in a vehicle (code done); entering one needs F7 (F is Minecraft's swap hands)
- [ ] Walls are found at chest height only (low obstacles come from the floor probes as steps)

## Features
- [x] SR3 HUD in Minecraft mode: weapon dial (with its health ring) and SR3's crosshair hidden via SR3's own display states (`hud_display_create_state` / `set_element` / `commit_state`, removed on leaving); minimap, money/respect, objectives stay (2026-10-04)
- [ ] SR3 damage to the player -> Minecraft hearts
- [ ] Signatures instead of fixed offsets (docs/RE.md) so game updates degrade gracefully

## Housekeeping
- [ ] `tools/smoke.py`: update for the new modes and movement, run end to end
- [ ] docs/RE.md: pause counter, raycast query/hit layout, camera update + hidden bit, object table, GetKeyboardState, teleport_to_object behaviour (lands on nearest surface; gravity), weapons Lua
