# CraftNet

CraftNet is a homebrew client for PlayStation Portable (PSP) that connects to Minecraft Java Edition multiplayer servers. A Windows build is also available from the same C++17 and raylib source.

CraftNet targets Minecraft Java Edition 26.3 (protocol 777) and PSP models with 64 MB of RAM. Connect to a compatible server that allows offline-mode login. Inventory support covers the player inventory and standard nine-column server menus. Microsoft sign-in and required server resource packs are currently unsupported.

## Features

- Saved server list with MOTD, player count, and version checks.
- Chunk streaming, block interaction, and chat with the PSP system keyboard.
- Hotbar, health, hunger, and server inventory menus with Minecraft textures.
- Player skins, NPCs, and floating labels.
- Dropped items, player armor, first-person swings, and server-driven respawn titles.
- Minecraft bitmap text and item tooltips with lore and enchantments.
- A PSP renderer with bounded mesh memory and precompiled block models.

## Run

Build and package the client using the instructions below.

**PSP.** Copy the full `dist/CraftNet-PSP` folder to `PSP/GAME/CraftNet` on your memory stick. Keep `assets/imported/compiled` beside `EBOOT.PBP`. Choose your Wi-Fi profile at startup, then add a server by hostname or IPv4 address and port.

**Windows.** Run `dist/CraftNet-Windows/CraftNet.exe`. The client starts in a maximized, resizable window with VSync off and no frame cap. Press **F11** to toggle fullscreen. The interface scales with the window.

| Action | PSP | Windows |
| --- | --- | --- |
| Move | Analog stick | WASD |
| Look | Triangle / Cross / Square / Circle | Mouse |
| Attack or mine | R | Left click |
| Use or place | L | Right click |
| Jump | Select | Space |
| Sneak | D-pad Right | Shift |
| Sprint | D-pad Left + Select | Ctrl |
| Switch hotbar slot | Hold D-pad Left, press L or R | 1–9 / Mouse wheel |
| Open inventory | D-pad Down | E |
| Navigate inventory | D-pad, Cross to select, Circle to close | Mouse or arrow keys, Enter to select, E to close |
| Drop item | D-pad Left + Down | Q |
| Chat | D-pad Up | T |
| Open game menu | Start | Esc |

The game menu shows the server's player list and lets you change video settings, remap gameplay controls, or disconnect. Settings are saved in `settings.dat` beside the game.

## Build

Use Windows PowerShell, Node.js, and MinGW GCC/G++. Set up the local dependencies before running the scripts.

| Path | Dependency |
| --- | --- |
| `.deps/mc26-data` | Minecraft 26.3 data reports |
| `.deps/zlib` | zlib source |
| `.deps/raylib-psp-6` | PSP port of raylib 6.0 |
| `.tools/pspdev` | PSP toolchain |
| `.tools/raylib-6.0_win64_mingw-w64` | raylib 6.0 for Windows |

```powershell
.\tools\import_assets.ps1 -ClientJar "C:\path\to\26.3.jar"
.\tools\build_block_atlas.ps1
.\tools\build_player_assets.ps1 -ClientJar "C:\path\to\26.3.jar"
.\tools\build_inventory_assets.ps1
.\tools\build_windows.ps1
.\tools\build_psp.ps1
.\tools\package.ps1
```

Use `-ReuseRaylib` on later PSP builds to reuse the compiled raylib objects. Build output goes to `build/` and packaged files go to `dist/`.

Import textures from your own Minecraft client JAR. Imported assets and build output are excluded from Git.

See the [development log](MODLOG.md) for implementation history.

## Third-party attribution

Parts of the protocol implementation, including the offline UUID routine, are derived from [mcprotocol.c](https://github.com/sk8erboi17/mcprotocol.c). The asset and item component generators use Minecraft 26.3 data from [mc26-data](https://github.com/mj41/mc26-data). The MIT terms below apply to these third-party materials.

```text
MIT License

Copyright (c) 2026 Giuseppe Perrotta (mcprotocol.c)
Copyright (c) 2026 mj41 (mc26-data)

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
