// SaintsCraft (SawyerTheNerd, MIT): real Minecraft 26.3 running alongside Saints Row: The Third Remastered and drawn
// into it (dinput8.dll proxy in the game folder + a Fabric mod, linked over 127.0.0.1:25599 and shared memory).
// No upstream release: SIGF built dinput8.dll and the Fabric jar from the pinned commit on a disposable AWS builder
// (library/QC.md section 4; source.json "built"), with one change: patches/0001-compile-out-lua-dev-channel.patch
// leaves out the Lua dev channel (devchan.cpp, luadev.cpp: any <game>\mcsr3\exec\*.lua ran in the game's Lua state
// with raw memory read/write helpers). Minecraft runs in the app's Prism instance, not as upstream's Gradle dev client.
//   SIGF_LIBRARY_BUILDS=<dir> node library/saintscraft/build.mjs      (outputs: library/lib.mjs)
import { instanceName } from '../../orchestrator/scripts/package-fusion.mjs';
import { mrpack, resolveFabricApi } from '../../orchestrator/src/recipe.js';
import { asset, card, dl, emit, zipAsset } from '../lib.mjs';
import { builtArtifacts, builtField, sourceOf } from '../um-gta5-passthrough/sigf-build.mjs';

const ID = 'saintscraft', VERSION = '0.1.0', NAME = 'SaintsCraft';
const SRC = sourceOf(ID);
const UP = { repo: SRC.repo, commit: SRC.commit, authors: ['SawyerTheNerd'] };
const MC = { mc: '26.3', loader: '0.19.5', fabricApi: '0.161.0+26.3', java: '25' }; // mc/gradle.properties at the commit
const JAR = 'passthrough-0.1.0.jar';
const PATCH = '0001-compile-out-lua-dev-channel.patch';
const TAGLINE = 'Real Minecraft inside Saints Row: The Third Remastered: walk Steelport as Steve, build, swing swords at the Saints\' enemies and blow up TNT, while SR3 keeps its cars.';

const files = builtArtifacts(ID);
// dinput8.dll into the game folder (SRTTR.exe loads it ahead of the system DLL), with the licenses and our patch.
const sr3 = zipAsset(`${ID}-sr3.zip`, [
  { name: 'dinput8.dll', data: files.get('dinput8.dll') },
  { name: 'mcsr3/LICENSE-saintscraft.txt', data: files.get('LICENSE') },
  { name: 'mcsr3/THIRD_PARTY_NOTICES-saintscraft.md', data: files.get('THIRD_PARTY_NOTICES.md') },
  { name: 'mcsr3/LICENSE-MinHook.txt', data: files.get('MinHook-LICENSE.txt') },
  { name: `mcsr3/sigf-build/${PATCH}`, data: files.get(PATCH) },
]);
const pack = async (offline) => {
  const fabricApi = offline ? null : await resolveFabricApi(MC.fabricApi, MC.mc);
  if (!offline && !fabricApi?.download) throw new Error(`Fabric API ${MC.fabricApi} not resolved on Modrinth`);
  return asset(`${ID}.mrpack`, mrpack({ name: NAME, summary: TAGLINE, versions: MC, versionId: VERSION, fabricApi,
    jars: [{ name: JAR, data: files.get(JAR) }], extra: [{ name: 'overrides/licenses/saintscraft-LICENSE.txt', data: files.get('LICENSE') }] }));
};
const assets = [sr3, await pack(false)];

const make = (urls, set) => {
  const mp = set.find(a => a.name.endsWith('.mrpack'));
  return {
    id: `sigf/${ID}`,
    version: VERSION,
    name: NAME,
    tagline: TAGLINE,
    kind: 'passthrough',
    games: [
      { game: 'saintsrow3', role: 'host', label: 'Saints Row: The Third Remastered', engine: 'Saints Row: The Third Remastered (DX11) + dinput8.dll proxy (C++, MinHook)', apps: { steam: '978300' }, runtime: 'Steam build of 2024-08 only (SRTTR.exe 21,474,968 bytes); Epic and GOG builds are not supported' },
      { game: 'minecraft', role: 'guest', label: 'Minecraft', engine: 'Minecraft Java 26.3 + Fabric mod passthrough (Java)', mc: MC.mc, loader: `fabric@${MC.loader}`, java: MC.java },
    ],
    requires: [
      { id: 'fabric-loader', version: MC.loader },
      { id: 'fabric-api', version: MC.fabricApi, note: 'in the Minecraft pack (downloaded from Modrinth)' },
    ],
    install: [
      { game: 'saintsrow3', strategy: 'game-dir-snapshot', files: [
        { src: sr3.name, dst: '{game}', unpack: true, contents: sr3.contents, ...dl(sr3, urls) },
      ] },
      { game: 'minecraft', strategy: 'mrpack', pack: { src: mp.name, ...dl(mp, urls) } },
    ],
    // Minecraft first (its mod serves the link on 127.0.0.1:25599), then Saints Row (upstream play.py's order).
    launch: [{ game: 'minecraft', wait: 'port:25599' }, { game: 'saintsrow3', args: [] }],
    files: set.map(a => ({ name: a.name, ...dl(a, urls) })),
    source: {
      repo: UP.repo, license: 'MIT AND BSD-2-Clause', upstream_license: SRC.license, commit: UP.commit,
      hosted: `https://github.com/SIGFAI/${ID}`,
      built: builtField(ID),
      patches: [{ file: `library/saintscraft/patches/${PATCH}`, why: 'Lua dev channel compiled out (CMake option MCSR3_DEVCHAN, default OFF)' }],
      linked: [{ name: 'MinHook', version: '1.3.4', repo: 'https://github.com/TsudaKageyu/minhook', commit: 'c3fcafdc10146beb5919319d0683e44e3c30d537', license: 'BSD-2-Clause' }],
    },
    media: {},
    built_by: { author: UP.authors[0], authors: UP.authors, packaged_by: 'SIGF' },
    idea_by: UP.authors[0],
    built_at: '2026-10-05T00:00:00.000Z',
    ...card(UP.repo),
    notes: [
      'You need Saints Row: The Third Remastered on Steam (the 2024-08 build; Epic and GOG builds are not supported) and Minecraft: Java Edition. Windows only, DirectX 11.',
      'The app adds dinput8.dll and a mcsr3 folder (licenses) to the Saints Row folder; Restore removes them. The mod writes its log to <game>\\mcsr3.',
      `Press Play: Minecraft starts first (the app's Prism instance "${instanceName(`sigf/${ID}`)}", Minecraft ${MC.mc}, Fabric Loader ${MC.loader}, Fabric API ${MC.fabricApi}, Java ${MC.java}, your own Minecraft account) and opens a void creative world by itself; leave its window open. Then Saints Row starts.`,
      'F7 switches between Minecraft mode (on foot, Steve, blocks, items) and Saints Row mode; in a vehicle it is always Saints Row mode.',
      'Single player only: do not join or host co-op while it is installed. Back up your saves first. Known upstream bugs: the SR3 HUD can vanish, scope zoom, cars only brake at blocks.',
      'SIGF build: the author\'s Lua dev channel (scripts dropped into mcsr3\\exec) is compiled out; nothing else differs from the upstream source.',
      'The link listens on 127.0.0.1:25599 with no authentication while Minecraft runs (upstream design).',
      `SIGF build of ${UP.commit.slice(0, 7)} (no upstream release; "Status: early"). Beta: report bugs to the author on the upstream issue tracker.`,
    ],
  };
};

emit({ slug: ID, version: VERSION, assets, make });
