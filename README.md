# Melee Board

Melee Board is an experimental native PC port of **Super Smash Bros. Melee**, built from the multiplatform Party Board/Aurora foundation.

The repository contains no Nintendo game assets. A legally obtained, uncompressed USA `GALE01` revision 2 (v1.02) GameCube disc image is required. The expected clean image is 1,459,978,240 bytes; its commonly published MD5 is `0e63d4223b01d9aba596259dc155a174`.

## Automatic Windows builds

Each push to `melee-port` starts a GitHub Actions build for Windows. Once it succeeds, the most recent **MeleeBoard-Windows-latest** artifact can be downloaded from the repository's Actions page. It contains the executable, its required runtime DLLs, and resources, but never a game disc image or Nintendo assets.

## Current state

The first porting milestone is implemented:

- the Party Board runtime is isolated on the `melee-port` branch;
- the launcher recognizes only Melee `GALE01` revision 2;
- Aurora mounts the selected disc image;
- the Mario Party 4 `game_main` and DOL-address import are disabled in the default build;
- a dedicated Melee bootstrap loop keeps the native window, renderer, input, and settings UI alive.
- a host-width-safe HSD object allocator, root class/object model, GObj
  scheduler, and logical GX frame setup are compiled and self-tested at startup.

This is not gameplay-ready yet. HSD integration has started; the next milestone is scene initialization, followed by Melee's scene system. See [docs/MELEE_PORT.md](docs/MELEE_PORT.md).

## Source reference

The gameplay and HAL code reference is the upstream [`doldecomp/melee`](https://github.com/doldecomp/melee) project, targeting USA v1.02. Keep that source separate from proprietary disc contents.

## Building on Windows

Install CMake 3.25+, Ninja, and the Visual Studio C++ workload, then initialize submodules and build:

```powershell
git submodule update --init --recursive
cmake --preset windows-msvc-relwithdebinfo
cmake --build --preset windows-msvc-relwithdebinfo
```

`MELEE_BOOTSTRAP=ON` is the default. It intentionally prevents the old Mario Party 4 entry point from running while the Melee runtime is being integrated.

## Credits

This work builds on Party Board, Aurora, the GameCube/Wii decompilation community, and the `doldecomp/melee` contributors.
