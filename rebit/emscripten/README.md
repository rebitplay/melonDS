# Rebit melonDS dual runtime

For the isolated view-independent correctness profile and its strict replicated
state tests, see [DETERMINISM.md](DETERMINISM.md). It is opt-in and is not a
production determinism or performance guarantee.

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

The WebAssembly interpreter uses a generated compact direct dispatcher, ported
from melonDS rebit `f0fd0ae`. Opcode indices select a small `u16` handler ID,
then a dense switch calls the instruction function directly. The existing
direct ARM9/ARM7 memory, cycle and branch helpers are retained. This is an
interpreter optimization, not a JIT or a renderer change.

Regenerate it after changing `src/ARM_InstrTable.h`, or verify it before a
release build. The tests check all 4096 ARM and 1024 Thumb mappings:

```bash
python3 rebit/emscripten/generate_arm_dispatch.py --check
node --test rebit/emscripten/tests/arm-dispatch.test.cjs
```

See [CPU_DISPATCH.md](CPU_DISPATCH.md) for the isolated build and A/B checks.
The full remelonds optimization inventory and WASM ARM7 fetch option are in
[RESEARCH_OPTIMIZATIONS.md](RESEARCH_OPTIMIZATIONS.md).

## Experimental rollback scheduler check

Rollback is opt-in (`REBIT_MELONDS_ROLLBACK=ON`), with the isolated build identity
`melonds-dual-rollback-experimental-2`. Frame completion participates in the
radio turn schedule so host thread timing cannot mark a console finished during
another console's handoff. The regression uses the actual scheduler without a
ROM and covers both seats and the host's reply-wait path:

```bash
cmake -S rebit/emscripten -B build/native-rollback -DREBIT_MELONDS_ROLLBACK=ON
cmake --build build/native-rollback --parallel
ctest --test-dir build/native-rollback --output-on-failure
```

This does not replace real-ROM restore/replay and public-CDN browser tests.
The default Lockstep and Android builds do not enable this profile.

## Historical cooperative WebAssembly build

The cooperative profile runs the replicated Local Wireless runtime without
pthreads, shared memory, or OffscreenCanvas. It also supports the single-console
host and firmware-client profiles used by Download Play over an external radio
transport.

```bash
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S rebit/emscripten -B build/wasm-cooperative -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DREBIT_MELONDS_DUAL_COOPERATIVE=ON
cmake --build build/wasm-cooperative --parallel
```

The cooperative artifacts are emitted under the same build directory with the
immutable build ID `melonds-dual-cooperative-2`; the publishing pipeline gives
them distinct filenames from the threaded profile.
