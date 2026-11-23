# Gauntlet Legends: Recompiled

A native PC port of **Gauntlet Legends** (Nintendo 64, USA), made by statically recompiling the game's
MIPS code to C with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and running it on
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) with the [RT64](https://github.com/rt64/rt64)
renderer. The same recompiled code also builds for the **PS Vita**, with its own renderer.

This repo contains no game data. You need your own **Gauntlet Legends (USA)** ROM; the setup recompiles the
game from it on your PC.

## Install (Windows 10/11, 64-bit)

1. Download this repo (green **Code** button, **Download ZIP**) and unzip it somewhere with a short path,
   such as `C:\Games\GauntletLegends`.
2. Drag your ROM (`.z64`, `.v64`, `.n64`, or a `.zip` with one in it) onto **`SETUP.bat`**. Or double-click
   `SETUP.bat` and it asks for the file.
3. When it says **All done**, start the game with **`PLAY.bat`**.

The setup installs nothing on Windows. Everything it needs is downloaded into the `deps` folder:

- the recompiler, runtime and renderer, at the versions this port was made with, plus the patches in `patches/`;
- a portable Python and Git if you don't have them;
- a portable LLVM compiler if you don't have one;
- Microsoft's C runtime and Windows SDK if Visual Studio isn't installed, fetched by the open-source
  [xwin](https://github.com/Jake-Shadle/xwin) tool under Microsoft's license.

The first run takes a while (about 3 GB of downloads, then the compile). Running it again skips the steps
that are already finished.

After updating (a new ZIP over the old folder, or `git pull`), run `SETUP.bat` again. It re-applies changed
patches and rebuilds what changed.

## Controls and saves

Controllers work through SDL (Xbox, PlayStation, Switch Pro and others). Each controller connected is a
player. Saves are kept on emulated Controller Paks in `port\build\data\saves`.

## PS Vita

Run **`BUILD_VITA.bat`**, or answer **y** when `SETUP.bat` offers the Vita version. It downloads VitaSDK into
`deps\vitasdk` and makes a `vita_files` folder with:

- `GauntletLegends.vpk`: install this with VitaShell (needs HENkaku/Enso);
- `ux0\data\GauntletLegends\rom.z64`: copy it to the same place on the Vita.

`vita_files\README.txt` has the details. Saves on the Vita go in `ux0:data/GauntletLegends/saves`.

## What's here

| Folder | |
|---|---|
| `port/` | the PC frontend: start-up, input, the RT64 hookup, game-specific fixes |
| `vita/` | the Vita frontend and its GXM renderer |
| `gfx/` | the portable F3DEX2/RDP renderer core the Vita build uses |
| `recomp/` | the recompiler configs: the function list (addresses only) and the audio microcode setup |
| `patches/` | changes to N64Recomp, N64ModernRuntime and RT64 |
| `setup/` | the installer |
| `tools/` | the steps that turn the ROM into the recompiler's input |

Nothing made from the ROM is ever committed: `rom/`, `recomp/RecompiledFuncs`, the builds and the VPK stay
on your PC (see `.gitignore`). Don't share them.

## Licence

The code in this repo is MIT licensed (see `LICENSE`). N64Recomp, N64ModernRuntime, RT64, SDL2 and VitaSDK
keep their own licences. Gauntlet Legends belongs to its rights holders; this project isn't affiliated with
them.
