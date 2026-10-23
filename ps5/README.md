# PS5 port

This is a native PS5 application, not a DOSBox package. Version 1.0.0-rc.1 is
the first release candidate. The first visit to weapon selection can take
longer than later visits. See the [root README](../README.md) for features,
DualSense controls, manual installation, and game-data instructions.

## Dependencies

Building requires the PS5 SDK/toolchain, `ps5-native-tool`, JWasm, the PS5 Port
Support Kit, and the private native SDL2 build used by this port. The local
Support Kit and SDK are not vendored here. Configure CMake with their paths and
then build:

```sh
cmake --build build/ps5-frontend --target bedlam2_native_eboot -j8
```

## Release package

The ProsperoStore catalog accepts a ZIP containing the app folder at the
archive root. Create one with:

```sh
python3 ps5/package_release.py \
  --eboot build/ps5-frontend/eboot.bin \
  --libc-prx /path/to/your/PS5/runtime/libc.prx \
  --output build/release/PPSA94003.zip
```

The script uses the committed `PPSA94003/sce_sys/` metadata and icon. The ZIP
contains only `eboot.bin`, the required runtime module, and `sce_sys/`; it never
copies `assets/bed2/`. The app expects user-supplied Bedlam 2 data at
`/app0/assets/bed2`, corresponding to
`/data/homebrew/PPSA94003/assets/bed2/` on the console.

The current native test build moves user files out of the replaceable app
folder: game data belongs in `/data/Bedlam2PS5/content/`, and generated saves
belong in `/data/Bedlam2PS5/saves/`. Startup creates both directories when
needed. When upgrading from the published RC.1 package, copy the game-data tree
from `/data/homebrew/PPSA94003/assets/bed2/` into `content/` and copy existing
`MIRAGE/AB_BED/` files into `saves/MIRAGE/AB_BED/` before replacing the app.
Keep the old app-folder copy until the new build has been confirmed.

`PORT_STATUS.md` and `NATIVE_DEBUG_HANDOFF.md` are local development notes and
are intentionally excluded from the public repository because they contain
console network details and machine-specific paths.
