"""Smoke test in the real game: restart SR3 with a fresh build (tools/devloop.py: waits until nobody's used the PC
for 2 minutes), then check each feature from the link log, Minecraft and screenshots. Minecraft must be running
(python play.py --mc).

  python tools/smoke.py            full run (restarts the game)
  python tools/smoke.py --no-restart
Prints PASS/FAIL per check, and leaves screenshots in tmp/smoke_*.png. Exit code 1 if any check failed.
"""
import os
import re
import subprocess
import sys
import time

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = yaml.safe_load(open(os.path.join(ROOT, "config.yaml")))
LOG = os.path.join(CFG["sr3"], "mcsr3", "mcsr3.log")
DUMPS = os.path.join(CFG["sr3"], "dumps")
results = []


def log():
    return open(LOG, errors="replace").read()


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  (' + detail + ')') if detail else ''}")


def lua(code):
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "srlua.py"), code], capture_output=True, text=True)
    return out.stdout.strip()


def mc(cmd):
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mccmd.py"), cmd], capture_output=True)


def shot(name):
    subprocess.run(["um", "win", "shot", "--exe", "SRTTR.exe", os.path.join(ROOT, "tmp", f"smoke_{name}.png"), "--scale", "0.5"], capture_output=True)


def drive(*cmds):
    subprocess.run(["um", "win", "drive", "--proc", "SRTTR", "focus", "scanmode on", *cmds], capture_output=True)


def wait_for(pattern, seconds):
    end = time.time() + seconds
    while time.time() < end:
        m = re.findall(pattern, log())
        if m:
            return m
        time.sleep(0.5)
    return []


def main():
    dumps_before = set(os.listdir(DUMPS)) if os.path.isdir(DUMPS) else set()
    if "--no-restart" not in sys.argv:
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "devloop.py")], check=True, capture_output=True)
    check("link connected to Minecraft", bool(wait_for(r"link: connected to Minecraft", 30)))
    check("scene projection found", bool(wait_for(r"depth: scene projection xs 1\.6", 30)))
    check("DirectInput hooked", "input: DirectInput devices hooked" in log())

    feet = lua("mclog(get_object_pos(LOCAL_PLAYER))").split("\t")[:3]
    check("player position from Lua", len(feet) == 3, " ".join(feet))
    people = wait_for(r"link: (\d+) SR3 people within", 15)
    check("SR3 people listed", bool(people), f"{people[-1] if people else '?'} nearby")
    ground = wait_for(r"groundscan: (\d+) ground blocks sent", 15)
    check("ground blocks sent", bool(ground) and int(ground[-1]) > 0, f"{ground[-1] if ground else 0} blocks")

    # Hotbar key and mouse buttons reach Minecraft (and not SR3)
    drive("key 0x32", "click 640 360")
    check("key 2 -> Minecraft hotbar", bool(wait_for(r"link: key 2 -> Minecraft hotbar", 5)))
    check("left mouse -> Minecraft", bool(wait_for(r"link: mouse attack -> Minecraft", 5)))
    drive("key 0x76")
    check("F7 -> Saints Row mode", bool(wait_for(r"link: Saints Row mode", 5)))
    drive("key 0x76")
    check("F7 -> Minecraft mode", len(re.findall(r"link: Minecraft mode", log())) >= 2)

    # TNT a few blocks in front of the player, lit by Minecraft: an SR3 explosion follows
    if len(feet) == 3:
        x, y, z = map(float, feet)
        mc(f"summon minecraft:tnt {x + 4:.2f} {y + 1:.2f} {-z:.2f} {{fuse:30}}")
        time.sleep(1.6)
        shot("tnt")
        # a failed spawn is logged with " (failed)" after the position, so it doesn't match
        check("Minecraft TNT -> SR3 explosion", bool(wait_for(r"link: Minecraft explosion r [\d.]+ -> SR3 \w+ at [-\d.]+ [-\d.]+ [-\d.]+\r?\n", 8)))

    time.sleep(20)  # run a while: the crash checks below need time
    shot("end")
    check("game still running", subprocess.run(["tasklist", "/FI", "IMAGENAME eq SRTTR.exe", "/NH"], capture_output=True, text=True).stdout.find("SRTTR.exe") >= 0)
    new_dumps = (set(os.listdir(DUMPS)) if os.path.isdir(DUMPS) else set()) - dumps_before
    check("no crash dumps", not new_dumps, ", ".join(sorted(new_dumps)))
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
