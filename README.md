# Bedlam 2: Absolute Bedlam — PS5

![BEDLAM 2 PS5](native/bedlam2-ps5-banner.png)

A native PS5 port of *Bedlam 2: Absolute Bedlam*, based on the source project by
[8street](https://github.com/8street/Bedlam2). It uses the game's native engine
with an SDL frontend and DualSense controls. Version 1.0.0 is in release-candidate
testing.

## Original game data is not included

You must provide the original DOS game data yourself. This repository and the
ProsperoStore package do **not** contain `BEDLAM.EXE`, the DOS extender, maps,
textures, audio, or other files from the original game distribution.

Use files from a copy you are legally entitled to use. The original project says
the game was never officially released; we do not know of an authorized public
download source and do not link to unofficial copies.

After installing the PS5 app, use your PS5 homebrew FTP server to copy the
contents of your game-data folder to:

```text
/data/homebrew/PPSA94003/assets/bed2/
```

Create `assets/bed2` if it is not already there. Put the game files directly in
that folder, so it contains entries such as `BEDLAM.EXE`, `GAMEGFX/`, `MIRAGE/`,
and `SOUND/` rather than another nested `bed2/` directory. The app reads them
from `/app0/assets/bed2`.

## Install on PS5

When the app is accepted into the Prospero homebrew catalog, install it through
ProsperoStore. Until then, download the
[1.0.0-rc.1 app ZIP](https://github.com/ChaDea1/Bedlam2-PS5/releases/download/01.000.000-rc.1/PPSA94003.zip),
extract it, and copy the
`PPSA94003` folder to `/data/homebrew/`, then register it with ShadowMountPlus.
Supply the original data at the path above before launching the game.

## Build

Building the native app requires a PS5 SDK/toolchain, the PS5 Port Support Kit,
and the private native SDL2 configuration used by this port. With those
dependencies configured in CMake, build the native executable with:

```sh
cmake --build build/ps5-frontend --target bedlam2_native_eboot -j8
```

The catalog release package is a ZIP of the `PPSA94003` app folder. Use
[`ps5/package_release.py`](ps5/package_release.py) to assemble one from a built
`eboot.bin` and your local PS5 runtime `libc.prx`; it deliberately does not add
game data. The checked-in title metadata is in `PPSA94003/sce_sys/`.

See [`ps5/README.md`](ps5/README.md) for PS5 development notes and known
limitations.

