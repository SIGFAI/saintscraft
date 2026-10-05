"""Dev loop: (re)start Saints Row with a freshly built link and load the latest save.

  python tools/devloop.py            close SR3 if running, build + install, launch, Continue
  python tools/devloop.py --no-build
  python tools/devloop.py --force     restart even if someone used the mouse/keyboard in the last 2 minutes
Uses um (universal-modder) for launching and input. Prints the link log when in game.
"""
import os
import subprocess
import sys
import time

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = yaml.safe_load(open(os.path.join(ROOT, "config.yaml")))
LOG = os.path.join(CFG["sr3"], "mcsr3", "mcsr3.log")


def sr3_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq SRTTR.exe", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith('"SRTTR.exe"'):
            return int(line.split('","')[1])
    return None


def log_text():
    try:
        return open(LOG, errors="replace").read()
    except OSError:
        return ""


def user_idle_seconds():
    out = subprocess.run(["um", "win", "drive", "--proc", "explorer", "idle"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith("idle ->"):
            return float(line.split()[-1])
    return 0.0  # no reading: assume someone is there


def main():
    # Never pull the game out from under a person, nor pop it up over what they're doing: wait until the PC
    # has been left alone.
    if "--force" not in sys.argv:
        while (idle := user_idle_seconds()) < 120:
            print(f"someone is using the PC (idle {idle:.0f} s): waiting")
            time.sleep(max(10, 120 - idle))
    pid = sr3_pid()
    if pid:
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
        while sr3_pid():
            time.sleep(0.5)
        time.sleep(2)  # Steam notices the exit
    if "--no-build" not in sys.argv:
        if subprocess.run([sys.executable, os.path.join(ROOT, "build.py")]).returncode:
            raise SystemExit("ERROR: build failed")
        subprocess.run([sys.executable, os.path.join(ROOT, "build.py"), "--install"], check=True)
    if os.path.isfile(LOG):
        os.remove(LOG)
    subprocess.run(["um", "win", "launch", "--steam", str(CFG["steam_appid"])], capture_output=True)
    end = time.time() + 180
    while time.time() < end and "mcsr3 link ready" not in log_text():
        time.sleep(1)
    if "mcsr3 link ready" not in log_text():
        raise SystemExit("ERROR: the link never started (see " + LOG + ")")
    # The main menu's first entry is CONTINUE (the latest save). Enter until a gameplay state exists.
    while time.time() < end and "gameplay state 0" not in log_text():
        time.sleep(4)
        subprocess.run(["um", "win", "drive", "--proc", "SRTTR", "focus", "key 0x0D"], capture_output=True)
    if "gameplay state 0" not in log_text():
        raise SystemExit("ERROR: no gameplay state after 3 minutes")
    time.sleep(8)  # the loading screen fades out
    print(log_text())


if __name__ == "__main__":
    main()
