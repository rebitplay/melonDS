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

## Native proof build

```bash
cmake -S rebit/emscripten -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel
./build/native/melonds_dual /authorized/game.nds 600 /tmp/melonds-dual
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
