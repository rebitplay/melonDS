# remelonds research → melonDS dual audit

Local implementation and verification, 2026-09-09. This inventory distinguishes
measured optimizations from unsuccessful experiments and architecture-specific
work. It does not treat every research candidate as a production best practice.

## Core coverage

| Research item | Status in dual |
| --- | --- |
| Direct ARM/Thumb handler calls | Already present; compact handler-ID dispatch ported in the preceding change. See [CPU_DISPATCH.md](CPU_DISPATCH.md). |
| Direct ARM9/ARM7 memory/cycle/branch helpers | Already present in `src/ARM.h`, matching donor `f0fd0ae`. |
| O3, LTO, WASM SIMD, pthreads | Already enabled by the Release Emscripten build. |
| DS RAM bus fast paths | Already present under `REBIT_MELONDS_NDS_ONLY`: both CPUs, multiple access widths, live RAM mappings. |
| ARM7 shared-WRAM instruction fetch | Ported as `REBIT_MELONDS_ARM7_FAST_FETCH`, WASM only; can be disabled for A/B builds. |
| CPU-island / dispatcher Asyncify pruning | Not applicable: dual's pthread runtime has no Asyncify transform/import suspension overhead to remove. Do not add the noinline/toolchain adapters without evidence. |
| ARM9 typed-ALU dispatch / noinline variants | Not selected: remelonds experiments showed no reliable gain. |
| Hot LDR routing / extra ARM9 main-RAM shortcut | Not selected: no reliable remelonds FPS gain; dual already has direct RAM bus paths. |
| Donor WebGL renderer | Not enabled: this software reference has no GL renderer, and the earlier WebGL path requires an optional extension unavailable in the user's default browser. |
| GL state cache/batching | A recommendation from profiling, not an implemented/validated remelonds optimization to copy. |
| SIMD snapshot copies / sleeping atomic waits / native WASM exceptions | Prior dual experiments were rejected; not re-enabled. |

The measured remelonds improvements are not additive and do not transfer as
percentages to a different core/backend. Dual already renders and schedules
multiple consoles in one module. The deterministic reference continues to
render all consoles synchronously, disables identical-frame skipping, and drains
audio at deterministic boundaries. Relaxing those settings requires separate
render-state/restore validation; it is not part of copying CPU optimizations.

## Fetch implementation

`ARMv4::CodeRead32` bypasses the general bus dispatch only for original-DS
addresses `0x03000000–0x037fffff`. It reads the current shared pointer/mask on
every access, uses private ARM7 WRAM when shared WRAM is unmapped, and preserves
word alignment and mirrors. Other regions and DSi delegate to the existing
reader with the original address. No decoded-instruction/value/mapping cache,
cycle change, IRQ change, or radio-timing change is introduced. The CMake
definition propagates to all consumers of the ARM header, avoiding inconsistent
class definitions. Native builds do not enable this option.

The focused test compiles the exact method extracted from `src/ARM.cpp` into
WASM and reuses the remelonds research fixture: 800,136 checks over live mappings,
null mapping, mirrors, edges/random/unaligned addresses, immediate writes and
fallback routing. DSi/peripheral routing uses a stub, not full-system validation.

## Frontend findings are separate

The rounded-deadline timer fix lives in the old one-console browser worker; it
is not a C++ core patch. The replicated playground has its own polling loop and
120-frame full-state hash barriers. This core-only pass does not alter those,
remove diagnostic integrity checks, or claim its measured throughput equals
browser FPS. Likewise the old product's startup-handoff fix and input-window
flow control are client lifecycle/netcode changes, not missing ARM emulation
optimizations. Existing private previews and their pinned core bytes remain
unchanged, as do production manifests and CDN artifacts.

## Reproduce

```sh
emcmake cmake -S rebit/emscripten -B build/wasm-research-optimizations \
  -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release \
  -DREBIT_MELONDS_ROLLBACK=ON -DREBIT_MELONDS_DETERMINISTIC=ON \
  -DREBIT_MELONDS_ARM7_FAST_FETCH=ON
cmake --build build/wasm-research-optimizations --parallel 4
node --test rebit/emscripten/tests/arm7-fetch.test.cjs \
  rebit/emscripten/tests/arm-dispatch.test.cjs \
  rebit/emscripten/tests/state-difference.test.cjs
```

The compiler can be selected with `EMSCRIPTEN=/path/to/emscripten` for the
focused test. It retains generated test artifacts in a new temporary directory.
Use the A/B harness from [CPU_DISPATCH.md](CPU_DISPATCH.md) with the previous
compact-dispatch binary as baseline and `NDS_CANDIDATE_LABEL=arm7-fetch`.
Run the existing strict race/boot rollback proof from
[DETERMINISM.md](DETERMINISM.md) against the candidate bytes. Timing runs must
not overlap builds or other emulator tests.

## Research sources

Sibling client reports: `docs/nds-wireless-memory-profile.md`,
`docs/nds-wireless-cpu-island.md`, `docs/nds-wireless-core-deep-profile.md`,
`docs/nds-wireless-arm9-noinline.md`, `docs/nds-wireless-instruction-profile.md`,
`docs/nds-wireless-mainram-experiment.md`, `docs/nds-wireless-render-profile.md`,
`docs/nds-wireless-pacing-fix.md`, `docs/nds-wasm-performance.md`, and
`docs/nds-local-wireless-performance.md`.

The historical compact-dispatch browser soak failed convergence in an older
profile. The newer deterministic profile's bounded race oracle passes, but
that is not grounds to erase the older failure or claim a production/browser
soak gate has been met. These remain local candidate artifacts.

## Measured fetch-port result

Build and seven ROM-free tests passed, including the 800,136 compiled WASM
fetch checks and all 5,120 opcode mappings. Same-machine Node v24.17.0, Linux
x64, Emscripten 3.1.74, two linked Mario Kart consoles, three paired repetitions
with 120 warm-up and 600 measured frames each, alternating execution order:

| Metric | Previous compact-dispatch build | Plus fetch port |
| --- | ---: | ---: |
| Pooled median core time | 21.634 ms | 21.301 ms |
| Mean core time | 21.651 ms | 21.318 ms |
| Core-only throughput | 46.19 FPS | 46.91 FPS |
| Run-level medians (ms) | 21.638 / 21.649 / 21.606 | 21.315 / 21.309 / 21.292 |

All three candidates were faster than all three controls within this sequence;
mean throughput improved approximately **1.56%**, not a 60-FPS result. The 60
cross-run full-state/video comparisons passed with no byte exclusions. This
does not establish browser/player FPS, cross-device performance or an isolated
instruction-level time saving. Host and workload variation still apply.

Compiler flags match apart from the fetch definition. Previous WASM:
`e3232325562097fe53b476eb68cc73b5206791efb7ef377e2e4f1422b1cb2d0f`.
Candidate WASM:
`0a745c5c1e72e12122305c40de90f2ac77c1edccfe00f74c1f086525c4d31efe`
(1,475,419 bytes). Raw samples and hashes are retained in
`build/wasm-research-optimizations/cpu-comparison.json`.

The option is ON for new WASM builds; set
`-DREBIT_MELONDS_ARM7_FAST_FETCH=OFF` to retain the previous fetch path. Native
builds remain on their existing path regardless of that CMake option. No live
artifact pin is changed by compiling the candidate.

Correctness on those exact bytes:

- 600-frame connected Mario Kart race checkpoint: 116 strict comparisons,
  38 wrong-branch restores, per-frame video/audio/save equality, and both
  corruption negative controls passed. Final full-state hashes match the
  pre-fetch baseline. See `build/wasm-research-optimizations/rollback-race.json`.
- 300-frame cold-boot tests with 2/3/4 consoles: 227 strict comparisons and
  75 restores each, all passed, with final hashes matching the previously
  recorded reference. These boot tests have no radio traffic and do **not**
  validate connected 3/4-player gameplay. Results: `rollback-boot-{2,3,4}.json`
  in the same build directory.
- The existing native scheduler test passed (0.91 seconds); native execution
  is unchanged. `git diff --check` passed. The original remelonds checkout is
  untouched.

This is bounded same-machine validation. Browser/device tests and a production
artifact rollout remain separate; no default-browser/WebGL or 60-FPS claim is
made by this pass.

## Follow-up: browser frame budget (2026-09-09–10)

The separate client pass improved worker pacing and tested the existing software
rasterizer threads under the deterministic oracle. All new CMake experiments
are OFF by default. The best measured candidate enables only
`REBIT_MELONDS_DETERMINISTIC_THREADED_RENDERER` on top of the fetch reference:

```sh
emcmake cmake -S rebit/emscripten -B build/wasm-threaded-render-experiment \
  -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release \
  -DREBIT_MELONDS_ROLLBACK=ON -DREBIT_MELONDS_DETERMINISTIC=ON \
  -DREBIT_MELONDS_ARM7_FAST_FETCH=ON \
  -DREBIT_MELONDS_DETERMINISTIC_THREADED_RENDERER=ON
cmake --build build/wasm-threaded-render-experiment --parallel 4
```

Its WASM SHA-256 is
`56be6c9c55052e1609dd596860ecfd9c4d6aed306387971c2c4efde9759df6c3`.
The eight-thread pool accommodates up to four console workers plus four
renderers. Every console renders, audio draining remains identical, rollback
waits for renderer quiescence, and the canonical serialized FrameIdentical flag
stays false. The 600-frame two-console race proof passed 116 strict comparisons,
38 restores and per-frame video/audio/save checks; final full-state hashes match
the synchronous reference. This does not prove connected four-console gameplay.

In two simultaneous Chromium contexts (two DS consoles each), the first
60-second run averaged 57.06 / 57.07 FPS. It is **not stable full DS speed**.
Detailed telemetry, screenshots and later RTT results live in the sibling
client's `docs/nds-replicated-60fps.md` and `artifacts/nds-60fps-*/`.

Three further opt-in experiments remain unselected:

- `REBIT_MELONDS_BATCH_RENDER_SCANLINES`: publish eight completed rows per
  notification, retaining one consumed token per line. No demonstrated gain.
- `REBIT_MELONDS_ADAPTIVE_WAIT`: bounded radio-turn spin then atomic wait.
  Slower in the browser test (~50.5 FPS each).
- `REBIT_MELONDS_REUSE_RASTER`: reuse upstream-eligible unchanged raster output
  using derived cache flags, without changing serialized FrameIdentical. Reset,
  invalidation and restore force fresh rasterization. No demonstrated gain
  (~56.8 FPS each); not selected or cold-boot multi-console validated.

All three passed the bounded 600-frame race proof with full-state final hashes
equal to the synchronous reference. Their correctness checks are not evidence
of a performance improvement. Original reference defaults, product identities,
CDN artifacts and the donor repository are unchanged.
