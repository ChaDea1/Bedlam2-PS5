# Native startup regressions

Run from the repository root. These tests use synthetic buffers, with no `bed2/`
assets and no console launch. They require a host C++ compiler and SDL2 headers.

## Low-address allocator

The kernel shim intentionally ignores `MAP_EXCL` so it can reproduce the old
allocator's overlapping mappings. With tracing disabled, the current allocator
must never submit a fixed mapping, must keep startup allocations disjoint, and
must safely handle invalid releases and mapping errors.

```sh
mkdir -p build/host-checks
g++ -std=c++17 -O1 -g -fsanitize=undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -DBEDLAM2_PS5_NATIVE=1 \
  -Ips5 -ICPP_Sources $(pkg-config --cflags sdl2) \
  ps5/tests/native_engine_memory_test.cpp ps5/native_engine_memory.cpp \
  -Wl,--gc-sections -pthread -o build/host-checks/native_engine_memory
build/host-checks/native_engine_memory
```

Function/data section garbage collection omits the unused assembly-engine
initialization code and its console-only symbols. The host test leaves tracing
disabled; the scratch-page probe is guarded by `BEDLAM2_PS5_TRACE`. To exercise
it on the host shim, add `-DBEDLAM2_PS5_TRACE=1 -DMAP_EXCL=0x4000` to the compiler
command and run the executable both normally and with `BEDLAM_TEST_HONOR_EXCL=1`.
These check the two probe outcomes without depending on the host's flag value.

## SDL software texture failure

Use the private SDL source copy with
`ps5/patches/sdl2-preserve-software-texture-error.patch` applied. This host build
uses a separate build directory from the native SDL installation.

```sh
cmake -S build/ps5-deps/sdl2-source -B build/host-checks/sdl \
  -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
  -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF \
  -DSDL_AUDIO=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF \
  -DSDL_LIBSAMPLERATE=OFF
cmake --build build/host-checks/sdl -j8
g++ -std=c++17 -O1 -g -DBEDLAM2_PS5_TRACE=1 -Ips5 \
  -Ibuild/host-checks/sdl/include -Ibuild/host-checks/sdl/include/SDL2 \
  -Ibuild/host-checks/sdl/include-config-release/SDL2 \
  ps5/tests/sdl_texture_allocation_test.cpp ps5/native_sdl_memory.cpp \
  build/host-checks/sdl/libSDL2.a -pthread -ldl -lm \
  -o build/host-checks/sdl_texture_allocation
build/host-checks/sdl_texture_allocation
```

The test rejects every large libc allocation and verifies that two simultaneous
1920x1080 RGBA buffers use native mappings instead. It checks zeroing, alignment,
overflow, migration from libc to mapped memory, failed realloc preserving the
original data, and complete mapped-buffer cleanup. Denying a native mapping
also verifies that SDL preserves the original `Out of memory` error.

## Console evidence

The `08a8c5e9b42c34ef` launch confirmed `MAP_EXCL` replaces a live scratch
mapping on this console. Game buffers were disjoint and all releases succeeded.
The first 8,294,440-byte SDL allocation succeeded, while the second failed with
reported errno 35; video init returned 1. libc termination then produced SIGSYS
inside libkernel, after the game's memory had been released. The revised build
uses native mappings for large SDL blocks and SystemService for termination.
The next `3e7ba9921346363a` launch validated the mapped heap through video
initialization. SystemService termination still requires console validation.
Preserve the staged binary's matching PIE and log offset before launching once.

## Execute-only engine data

The `3e7ba9921346363a` console launch passed video and input initialization,
including both 1080p allocations. It then faulted while reading `byte_14723`
from executable text during engine-memory initialization. The converter now
moves 128 data symbols into the readable, mirrored engine data image, including
117 switch tables and two codec tables. Instruction bytes and the DOS interrupt
trampoline remain in text for the separate executable-code audit.

After building the native PIE, run:

```sh
python3 ps5/tests/check_engine_readable_data.py
```

This checks section permissions and mirror bounds for every relocated symbol,
all 1,340 switch/codec code offsets against their actual destinations, the
lookup table bytes and adjacency, and the absence of text-base addressing for
the moved data. Table destinations are global labels in assembly so the data
section can refer to labels that previously had procedure-local scope.

## Engine stacks and external-call ABI

```sh
python3 ps5/tests/run_engine_shim_test.py
```

This builds the actual native entry shim, external-call thunk assembly, and
guarded-stack allocator with a small synthetic legacy engine. It calls the
production directory bridge through the four-byte argument adapters, verifies the
main argc/argv/envp slots and all SysV callee-saved registers, and runs the timer
and main entries concurrently on separate low stacks. Child processes verify
that both ends of both stacks fault on access. The allocator is also checked
for retry after failed guard protection. UndefinedBehaviorSanitizer is enabled.
Unrelated application functions are trapping test stubs, so accidental calls
cannot silently pass. A source check covers all 179 named formal-argument
declarations: return-address growth shifts them by four while locals stay put.

The engine reserves R12 for restoring the host stack. Generated engine code
must not use R12; the external-call adapters preserve it. Unnamed stack
references and other legacy stack transformations still need separate audit.

The current test links the production file bridge and exercises both the
getcwd and mkdir thunks on the low stack. It checks the engine's absolute
virtual directory, relative/backslash path normalization, buffer bounds,
native mkdir permissions and error conversion, and agreement with file lookup.
The kernel mkdir shim verifies the exact path without creating directories.
The earlier version called host libc getcwd; that proved the ABI on Linux but
did not validate the console's imported function pointer.
