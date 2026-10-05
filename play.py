"""Start Minecraft X Saints Row 3: the Minecraft dev client (mc/) and Saints Row with the link.

  python play.py           start Minecraft, then Saints Row (Steam); stops Minecraft when SR3 closes
  python play.py --mc      start only Minecraft (SR3 already running, or started some other way)
Install the SR3 side first: python build.py --install
"""
import glob
import os
import subprocess
import sys
import time

import yaml

ROOT = os.path.dirname(os.path.abspath(__file__))
CFG = yaml.safe_load(open(os.path.join(ROOT, "config.yaml")))
STEAM = r"C:\Program Files (x86)\Steam\steam.exe"


def fail(msg):
    raise SystemExit(f"ERROR: {msg}")


def running(image):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/NH"], capture_output=True, text=True).stdout
    return image.lower() in out.lower()


def start_minecraft():
    jdks = sorted(glob.glob(os.path.join(ROOT, "tools", "jdk-25*")))
    if not jdks:
        fail("no JDK 25 in tools/: run tools/fetch_sources.sh")
    env = dict(os.environ, JAVA_HOME=jdks[-1])
    log = open(os.path.join(ROOT, "minecraft.log"), "w")
    print("Minecraft (dev client) starting; the first run downloads it (a few minutes). Log: minecraft.log")
    gradlew = os.path.join(ROOT, "mc", "gradlew.bat")  # full path: cmd may not search the working directory
    return subprocess.Popen(["cmd", "/c", gradlew, "runClient", "--no-configuration-cache", f"-Dpassthrough.name={CFG.get('minecraft_name', 'Steve')}"],
                            cwd=os.path.join(ROOT, "mc"), env=env, stdout=log, stderr=subprocess.STDOUT)


def main():
    if not os.path.isfile(os.path.join(CFG["sr3"], "dinput8.dll")):
        fail("the link isn't installed: python build.py --install")
    mc = start_minecraft()
    if "--mc" in sys.argv:
        return
    if running("SRTTR.exe"):
        print("Saints Row is already running")
    else:
        subprocess.Popen([STEAM, "-applaunch", str(CFG["steam_appid"])])
        print("Saints Row starting; link log: " + os.path.join(CFG["sr3"], "mcsr3", "mcsr3.log"))
        for _ in range(120):
            if running("SRTTR.exe"):
                break
            time.sleep(1)
    while running("SRTTR.exe") and mc.poll() is None:
        time.sleep(2)
    if mc.poll() is None:
        print("Saints Row closed: stopping Minecraft")
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(mc.pid)], capture_output=True)
    else:
        print(f"Minecraft exited (code {mc.returncode}); see minecraft.log")


if __name__ == "__main__":
    main()
