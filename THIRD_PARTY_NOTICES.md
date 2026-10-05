# Third-party notices

This project builds on other people's work. Their licences and notices are below.

## Code this project includes or is adapted from

### universal-modder: Minecraft x GTA V passthrough example (MIT)
https://github.com/rehan-remade/universal-modder

The Minecraft mod in `mc/` (Fabric mod, WebSocket host link, frame export to shared memory, input bridge) and
the link's WebSocket client and frame reader started from this example.

```
MIT License

Copyright (c) 2026 Rehan and universal-modder contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### SkyCraft (MIT)
https://github.com/chasmlol/SkyCraft

The idea (real Minecraft inside another game) and its Half-Life port; `InputBridge.java` is adapted from
SkyCraft's.

```
MIT License

Copyright (c) 2026 chasmlol

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Downloaded at build time (not included here)

- **MinHook** by Tsuda Kageyu (BSD 2-Clause): https://github.com/TsudaKageyu/minhook. CMake fetches it when
  `link/` is built; its own licence comes with it.
- **Fabric Loader / Fabric API, Minecraft**: fetched by Gradle from their official repositories for the dev
  client. Minecraft itself is Mojang's and is downloaded from Mojang by the build, never included here.

## References (facts and formats, no code copied)

- **SaintExec** by Nathnefo (GPL-3.0): https://github.com/Nathnefo/SaintExec. The byte patterns used to find
  SR3's Lua API functions, and the idea of answering "current script thread" for code run outside a script.
- **ThomasJepp.SaintsRow** by Thomas Jepp: https://github.com/saintsrowmods2/ThomasJepp.SaintsRow. The packfile
  (`.vpp_pc`) format that `tools/vpp.py` reads (re-implemented in Python, read only).
- **Kinzie's Toy Box** (saintsrowmods.com): documentation of SR3's gameplay script functions.

## The games

Saints Row: The Third Remastered belongs to Deep Silver / Volition, and Minecraft to Mojang Studios / Microsoft.
No game files are in this repository; you need your own copies of both games.
