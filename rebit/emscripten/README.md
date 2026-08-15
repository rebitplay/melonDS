# Rebit melonDS dual runtime

This is an isolated standalone melonDS frontend for Rebit's browser-native
Nintendo DS Local Wireless mode. It does not use RetroArch or melonDS DS.

The runtime owns 2–4 standalone `melonDS::NDS` consoles, drives one frame on
each console concurrently, and connects their emulated radios through
standalone melon's in-process `LocalMP` interface. Every browser runs the same
set of consoles; only deterministic controller/touch input is synchronized
between browsers. Each seat has an independent SRAM import/export path so the
owning player's save is loaded before the first synchronized frame and can be
persisted again at a session barrier.

All production implementations use the implementation-neutral engine identity
`melonds-dual-parallel-1` and runtime ABI `rebit-melonds-parallel-v1`. The
WebAssembly and Android builds must remain state/checkpoint compatible so a
room never depends on the participant's operating system.

## Native SDK and proof build

```bash
cmake -S rebit/emscripten -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel
./build/native/melonds_dual /authorized/game.nds 600 /tmp/melonds-dual
```

The reusable target is `rebit_melonds_dual_runtime`; its stable C API is
published in `include/rebit_melonds_dual.h`. Disable the CLI with
`-DREBIT_MELONDS_DUAL_BUILD_CLI=OFF` when embedding the library.

## Android SDK

`../android-sdk` is an Android library module for API 29+. It builds the same
parallel interpreter runtime and supplies a thin JNI interface. JIT execution
is deliberately disabled because it does not reproduce the WebAssembly state
hash; native threaded interpreter execution retains deterministic checkpoints
while providing the required performance headroom. The application layer owns
the Capacitor plugin and surface placement; the SDK owns emulation, EGL
presentation, and AAudio output.

The Android build supports `arm64-v8a` and `x86_64` and is pinned to NDK
28.2.13676358. It can be included directly from a Gradle settings file:

```groovy
include ':rebit-melonds-native'
project(':rebit-melonds-native').projectDir = file('path/to/melonDS/rebit/android-sdk')
```

## WebAssembly build

Emscripten 3.1.74 or newer is required. The web application must serve the
runtime under COOP/COEP because the local radio waits require pthreads and
`SharedArrayBuffer`.

```bash
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S rebit/emscripten -B build/wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/wasm --parallel
```

Artifacts are written to `build/wasm/dist/melonds_dual.{js,wasm}`. Emscripten
embeds the pthread bootstrap in the modularized JavaScript wrapper.

The WebAssembly interpreter uses a generated direct dispatcher. Regenerate it
after changing `src/ARM_InstrTable.h`, or verify it before a release build:

```bash
python3 rebit/emscripten/generate_arm_dispatch.py --check
```

## Historical cooperative WebAssembly build

The cooperative profile is retained only for reproducibility of the rejected
mobile experiment. It does not meet Rebit's 59 FPS release gate and must not be
used for production rooms or new releases.

```bash
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S rebit/emscripten -B build/wasm-cooperative -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DREBIT_MELONDS_DUAL_COOPERATIVE=ON
cmake --build build/wasm-cooperative --parallel
```

The cooperative artifacts are emitted under the same build directory with the
immutable build ID `melonds-dual-cooperative-1`; the publishing pipeline gives
them distinct filenames from the threaded profile.
