"""Run Lua inside Saints Row's gameplay state (dev channel of the mcsr3 link DLL).

  python tools/srlua.py "mclog(LOCAL_PLAYER)"     run a snippet, print what mclog() printed
  python tools/srlua.py -f probe.lua              run a file
Exit code 1 if the chunk raised an error. Needs the game in a loaded save with the link installed.
"""
import argparse
import os
import sys
import time

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXEC = os.path.join(yaml.safe_load(open(os.path.join(ROOT, "config.yaml")))["sr3"], "mcsr3", "exec")


def run(code, timeout=10.0):
    """Returns (ok, output). Raises TimeoutError if the game never picked it up (not in game?)."""
    name = f"job{os.getpid()}_{time.time_ns()}"
    out = os.path.join(EXEC, name + ".out")
    tmp = os.path.join(EXEC, name + ".part")
    with open(tmp, "w", newline="\n") as f:
        f.write(code)
    os.replace(tmp, os.path.join(EXEC, name + ".lua"))
    end = time.time() + timeout
    while time.time() < end:
        if os.path.isfile(out):
            text = open(out).read()
            os.remove(out)
            status, _, body = text.partition("\n")
            return status == "ok", body
        time.sleep(0.02)
    lua = os.path.join(EXEC, name + ".lua")
    if os.path.isfile(lua):
        os.remove(lua)
    raise TimeoutError("no reply: is the game in a loaded save with the link installed?")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("code", nargs="?")
    ap.add_argument("-f", "--file")
    ap.add_argument("-t", "--timeout", type=float, default=10.0)
    a = ap.parse_args()
    code = open(a.file).read() if a.file else a.code
    if not code:
        ap.error("give code or -f file")
    ok, out = run(code, a.timeout)
    sys.stdout.write(out)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
