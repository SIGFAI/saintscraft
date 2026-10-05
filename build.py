"""Build the SR3 side of Minecraft X Saints Row 3 (link/ -> dinput8.dll) and install it.

  python build.py              build only (build/Release/dinput8.dll)
  python build.py --install    build, then copy dinput8.dll next to SRTTR.exe
  python build.py --uninstall  remove what --install added
Needs Visual Studio 2022 (Build Tools) with the C++ workload, and CMake.
"""
import os
import shutil
import subprocess
import sys

import yaml

ROOT = os.path.dirname(os.path.abspath(__file__))
CFG = yaml.safe_load(open(os.path.join(ROOT, "config.yaml")))
SR3 = CFG["sr3"]
BUILD = os.path.join(ROOT, CFG.get("build", "build"))
INSTALLED = ["dinput8.dll"]  # files --install puts in the game folder (mcsr3/ holds logs only)


def fail(msg):
    raise SystemExit(f"ERROR: {msg}")


def sr3_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq SRTTR.exe", "/NH"], capture_output=True, text=True).stdout
    return "SRTTR.exe" in out


def build():
    if not os.path.isfile(os.path.join(BUILD, "CMakeCache.txt")):
        subprocess.run(["cmake", "-S", os.path.join(ROOT, "link"), "-B", BUILD, "-G", "Visual Studio 17 2022", "-A", "x64"], check=True)
    r = subprocess.run(["cmake", "--build", BUILD, "--config", "Release", "--", "/m", "/v:minimal", "/nologo"])
    if r.returncode:
        fail("build failed (see the compiler output above)")
    return os.path.join(BUILD, "Release", "dinput8.dll")


def install(dll):
    if not os.path.isfile(os.path.join(SR3, "SRTTR.exe")):
        fail(f"SRTTR.exe not found in {SR3}: set sr3 in config.yaml")
    if sr3_running():
        fail("Saints Row is running: close it first (the DLL is in use)")
    shutil.copy2(dll, os.path.join(SR3, "dinput8.dll"))
    print(f"installed {os.path.join(SR3, 'dinput8.dll')}")


def uninstall():
    if sr3_running():
        fail("Saints Row is running: close it first")
    for name in INSTALLED:
        p = os.path.join(SR3, name)
        if os.path.isfile(p):
            os.remove(p)
            print(f"removed {p}")


def main():
    if "--uninstall" in sys.argv:
        return uninstall()
    dll = build()
    if "--install" in sys.argv:
        install(dll)


if __name__ == "__main__":
    main()
