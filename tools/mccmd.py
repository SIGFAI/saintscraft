"""Send messages to the Minecraft mod's host link (ws://127.0.0.1:25599), like the SR3 link does.

  python tools/mccmd.py "setblock 0 64 0 stone"         a server command
  python tools/mccmd.py --raw '{"t":"slot","n":2}'      any host message
Prints what Minecraft sends back within --wait seconds (default 0.5).
"""
import argparse
import asyncio
import json

import websockets


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--wait", type=float, default=0.5)
    a = ap.parse_args()
    msg = a.command if a.raw else json.dumps({"t": "cmd", "c": a.command})
    async with websockets.connect("ws://127.0.0.1:25599") as ws:
        await ws.send(msg)
        # Minecraft also broadcasts a steady stream (mobs, projectiles): stop after --wait seconds overall
        end = asyncio.get_event_loop().time() + a.wait
        try:
            while (left := end - asyncio.get_event_loop().time()) > 0:
                print(await asyncio.wait_for(ws.recv(), left))
        except asyncio.TimeoutError:
            pass


asyncio.run(main())
