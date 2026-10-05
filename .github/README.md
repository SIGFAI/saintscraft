# SaintsCraft

Real Minecraft inside Saints Row: The Third Remastered: walk Steelport as Steve, build, swing swords at the Saints' enemies and blow up TNT, while SR3 keeps its cars.

**SaintsCraft is made by [SawyerTheNerd](https://github.com/SawyerTheNerd).** All credit for the mod goes to them.

- Original project: https://github.com/SawyerTheNerd/Minecraft-X-SaintsRow3Remastered
- Report bugs and ask questions there: https://github.com/SawyerTheNerd/Minecraft-X-SaintsRow3Remastered/issues
- Upstream version packaged here: 0.1.0 (commit [`f608f69`](https://github.com/SawyerTheNerd/Minecraft-X-SaintsRow3Remastered/tree/f608f6962ec809ce2b2d97df9898817998611bde))
- **Built by SIGF from commit [`f608f6962ec809ce2b2d97df9898817998611bde`](https://github.com/SawyerTheNerd/Minecraft-X-SaintsRow3Remastered/tree/f608f6962ec809ce2b2d97df9898817998611bde)** with the changes described below, on a disposable build machine (AWS EC2 i-0022be8314df37721 (c6i.4xlarge, Windows Server 2022, terminated after the build)). The app installs these SIGF builds, not binaries from the author.

> **Beta.** Nobody at SIGF has played this build yet. Back up your saves.
> Bugs in the mod itself go to the author's issue tracker above; problems with the one-click install go to this repository's issues.

## What you need

- **Saints Row: The Third Remastered** ([Steam](https://store.steampowered.com/app/978300/)): Steam build of 2024-08 only (SRTTR.exe 21,474,968 bytes); Epic and GOG builds are not supported.
- **Minecraft**: Java Edition 26.3.
- Windows and the [SIGF app](https://sigf.ai). The app installs fabric-loader 0.19.5, fabric-api 0.161.0+26.3 for you.

## Install

In the SIGF app, open **SaintsCraft** in the catalog, press **Install**, then **Play**. **Restore** puts your game folders back exactly as they were.
The app follows `mashup.json` in this repository: every download is pinned by sha256. The files come from the release [`v0.1.0`](../../releases/tag/v0.1.0).

### Good to know

- You need Saints Row: The Third Remastered on Steam (the 2024-08 build; Epic and GOG builds are not supported) and Minecraft: Java Edition. Windows only, DirectX 11.
- The app adds dinput8.dll and a mcsr3 folder (licenses) to the Saints Row folder; Restore removes them. The mod writes its log to <game>\mcsr3.
- Press Play: Minecraft starts first (the app's Prism instance "sigf-saintscraft", Minecraft 26.3, Fabric Loader 0.19.5, Fabric API 0.161.0+26.3, Java 25, your own Minecraft account) and opens a void creative world by itself; leave its window open. Then Saints Row starts.
- F7 switches between Minecraft mode (on foot, Steve, blocks, items) and Saints Row mode; in a vehicle it is always Saints Row mode.
- Single player only: do not join or host co-op while it is installed. Back up your saves first. Known upstream bugs: the SR3 HUD can vanish, scope zoom, cars only brake at blocks.
- SIGF build: the author's Lua dev channel (scripts dropped into mcsr3\exec) is compiled out; nothing else differs from the upstream source.
- The link listens on 127.0.0.1:25599 with no authentication while Minecraft runs (upstream design).
- SIGF build of f608f69 (no upstream release; "Status: early"). Beta: report bugs to the author on the upstream issue tracker.

## SIGF changes

SIGF built `dinput8.dll` and the Fabric jar from the pinned commit with one patch, `sigf/patches/0001-compile-out-lua-dev-channel.patch`: the Lua developer channel is compiled out (CMake option `MCSR3_DEVCHAN`, default OFF). Nothing else differs from upstream's source.

## What this repository holds

1. The upstream source tree at commit [`f608f6962ec809ce2b2d97df9898817998611bde`](https://github.com/SawyerTheNerd/Minecraft-X-SaintsRow3Remastered/tree/f608f6962ec809ce2b2d97df9898817998611bde), every file unchanged (same git blobs). Upstream's own `README.md` is there, unchanged; GitHub shows this file (`.github/README.md`) first.
2. Added by SIGF in the same commit: this file, `THIRD-PARTY.md` (licenses and sources of the third-party files in the release), `sigf/patches/` (our patch, applied before the build), and `sigf/` (the scripts that built the release assets, for reference: they run inside the SIGF repository).
3. `mashup.json`, the SIGF app recipe (the next commit).
4. The release `v0.1.0` (its tag is the first commit):

| Asset | Size | sha256 | What it is |
|---|---|---|---|
| `saintscraft-sr3.zip` | 196006 B | `4ee627201dfe232ac684355d9294a45a51eefae5c4df69eb3be3dfc74b05eceb` | the SIGF build of `dinput8.dll` (statically linked with MinHook 1.3.4, BSD-2-Clause) from the pinned commit with our patch, and under `mcsr3/` upstream's LICENSE and THIRD_PARTY_NOTICES, the MinHook license and the patch; into the Saints Row: The Third Remastered folder. |
| `saintscraft.mrpack` | 216989 B | `6303b5b8d2127636c7c3537e286f6df4eca4e8d14c1ed65b9d8d2f5e7e86986e` | the Minecraft side: the SIGF build of `passthrough-0.1.0.jar` from the pinned commit, with upstream's LICENSE, for Minecraft 26.3 with Fabric Loader 0.19.5; Fabric API 0.161.0+26.3 is a Modrinth download link, not stored here. |

The sha256 of every file inside the zips is in `mashup.json` (`contents`).

## Licenses

| Part | License | Where |
|---|---|---|
| Minecraft-X-SaintsRow3Remastered (all of the upstream tree, and the SIGF builds) | MIT, Copyright SawyerTheNerd | `LICENSE`, `THIRD_PARTY_NOTICES.md` |
| MinHook 1.3.4 (linked into `dinput8.dll`) | BSD-2-Clause | `THIRD-PARTY.md` |
| Fabric API (downloaded from Modrinth by the app, not stored here) | Apache-2.0 | https://github.com/FabricMC/fabric |

## Why this repository exists

The SIGF app (https://sigf.ai) installs mods from recipes (`mashup.json`) whose downloads are pinned release files. This repository makes SaintsCraft installable in one click, credited to SawyerTheNerd. If you are the author and want anything changed or taken down, open an issue here.
