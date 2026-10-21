# Dino Crisis ME Studio

A mod tool for [Dino Crisis - Modern Edition](../../../Dino-Crisis-Modern-Edition). Opens your Dino Crisis disc image, shows the game's characters, rooms, textures, sounds and videos, exports them as ordinary files (models as glTF, FBX or OBJ) and builds your changes into the game's mods folder. The running game picks the changes up by itself.

**Download:** `DinoCrisisME_Studio.zip` on the [Releases](../../releases) page.

## Use

1. Install Dino Crisis ME.
2. Unpack the studio anywhere. Inside the game folder the studio finds the game's mods folder by itself, elsewhere it asks for it once.
3. Run `DinoCrisisME_Studio.exe` and open the Dino Crisis (USA) v1.1 disc image (Track 1 `.bin` or `.cue`).

Windows 10 or 11, 64-bit, OpenGL 3.3.

## Mod packages

Each mod in its own folder: `mods\<name>\` with `mod.json` and the same folders inside (textures, characters, rooms...). The game reads every package that is on; a file two packages have is taken from the lower one in the list.

Click **mods** at the top: on and off, order, New, Install .zip, Build .zip. Convert all and the room editors write into the highlighted line.

## Build

- Visual Studio 2022 or newer (C++17), CMake 3.20 or newer
- SDL2 2.30.3 development package for Visual C++ (`SDL2-devel-2.30.3-VC.zip`), unpacked to `../sdl2-msvc/SDL2-2.30.3` next to this folder, or pass `-DSDL2_DIR_ROOT=<path>`

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `build\Release\DinoCrisisME_Studio.exe` and the command-line `DinoCrisisME_Studio_cli.exe`.

## Licence

MIT (`LICENSE`). Libraries: `THIRD-PARTY.txt` (SDL2, Dear ImGui, nlohmann/json, stb).

Dino Crisis belongs to Capcom. This is a fan tool, not made or endorsed by Capcom, and nothing of the game is inside. What you export stays Capcom's: share only what you made yourself.
