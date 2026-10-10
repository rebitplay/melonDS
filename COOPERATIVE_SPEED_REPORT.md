# Default NDS WASM core speed experiments — 2026-10-10

This follows the [WASM netplay research](../melonds-wasm-netplay-research/WASM_NETPLAY_REPORT.md)
and tests actual core changes against Rebit's default **cooperative-v1**,
`melonds-dual-cooperative-3`, runtime ABI `rebit-melonds-dual-cooperative-2`.
The optional threaded `deterministic-v1` core is outside this experiment.

**Follow-up:** [full CPU specialization](CPU_SPEED_REPORT.md) subsequently
reduced mean core time by about **5.6%** in the longer matched-control test.
The small renderer/dispatch experiments below did not establish a performance
ceiling; their original measurements are retained here.

**Result:** explicit integer SIMD saved about **0.5% core time** in the longer
test, from **17.809 to 17.720 ms**. This is a small experimental improvement on
one desktop/game fixture, insufficient to reach the roughly 16.72 ms DS budget.
Keep it available for further validation; do not enable it by default based on
this result alone. The other two candidates were slower.

## What was implemented

Three CMake experiments are isolated on branch
`experiment/cooperative-core-speed-20261010`, based on
`0cb05783dc3d9c9032aff0c85ec6fc944c808663`. They are all **OFF by default**.
No client, netcode, negotiated input delay, shipped core, or CDN artifact was changed.

| Option | Change | Initial result |
| --- | --- | --- |
| `REBIT_MELONDS_LATE_PERSPECTIVE` | Delay the per-pixel perspective division until Z-buffer depth passes; preserve W-buffer calculation before depth testing | No improvement |
| `REBIT_MELONDS_INDIRECT_DISPATCH` | Use the canonical ARM/Thumb function-pointer tables instead of the existing compact direct dispatcher | Slower |
| `REBIT_MELONDS_SIMD_ATTRIBUTES` | Interpolate red, green, blue and the S texture coordinate together using standard WASM integer SIMD | About 0.5% less core time in longer confirmation |

The SIMD path uses the same integer multiply, unsigned shift, signed base and
rounding as the scalar path. Linear interpolation retains the original signed
64-bit divisions. T texture interpolation and depth calculation remain scalar.
Both DS consoles still render; capture, radio scheduling, CPU cycles and stored
emulator state retain their existing behavior. This is explicit vectorization
inside a hot loop, not merely enabling `-msimd128`, which the core already uses.

Source changes are in [GPU3D_Soft.h](src/GPU3D_Soft.h),
[GPU3D_Soft.cpp](src/GPU3D_Soft.cpp), [ARM.cpp](src/ARM.cpp) and
[CMakeLists.txt](rebit/emscripten/CMakeLists.txt).

## Performance evidence

All measurements are `_md_run_frame()` wall time in headless desktop Chromium,
with two concurrent browser workers. **Each worker emulates both linked DS
consoles**; these represent two replicas running on the same workstation.
The two seats are not independent benchmark repetitions.

The first screen uses two trials per variant, 720 frames per trial, with the
first 120 excluded from timings. All use the same Mario Kart DS race checkpoint
and deterministic frame-indexed inputs. Variant order was
`pinned/control/late/indirect/indirect/late/control/pinned`.

| Variant | Mean core time | p95 | Change against rebuilt control |
| --- | ---: | ---: | ---: |
| Pinned current core | 17.719 ms | 19.5 ms | — |
| Fresh O3 control | 17.713 ms | 19.5 ms | — |
| Late perspective calculation | 17.818 ms | 19.7 ms | 0.59% slower |
| Function-pointer dispatch | 18.785 ms | 20.7 ms | 6.05% slower |

Retain the current dispatcher and eager perspective calculation. Passing output
comparisons is necessary, but does not make a slower candidate useful.

A separate `pinned/SIMD/SIMD/pinned` screen gave:

| Variant | Mean core time | Trial pair means |
| --- | ---: | --- |
| Pinned | 17.770 ms | 17.778 / 17.761 ms |
| SIMD attributes | 17.650 ms | 17.601 / 17.699 ms |

That is a **0.67% reduction in core time** in the short screen. Its size calls
for confirmation; it does not establish a general performance gain.

### Full-build confirmation

Six alternating 1,800-frame trials completed against a complete CMake build of
the SIMD candidate, with 120 warmup frames and 1,680 measured frames per seat.
Order: `pinned/SIMD/SIMD/pinned/pinned/SIMD`.

| Variant | Mean core time | p95 | Trial pair means |
| --- | ---: | ---: | --- |
| Pinned | 17.809 ms | 19.5 ms | 17.827 / 17.754 / 17.845 ms |
| Full SIMD build | 17.720 ms | 19.4 ms | 17.688 / 17.742 / 17.730 ms |

The aggregate saving is **0.089 ms, or 0.50%**. Matched repetitions differ by
0.78%, 0.07% and 0.65%, so the effect is small relative to run variation. There
are three trial pairs, not thousands of independent samples; no statistical
confidence interval or general device/game speedup is established. All **180
sampled output records** agree with the pinned same-seat reference. The mean
still needs about **5.6% less core time** to reach 16.72 ms, with additional
headroom needed for product overhead and slow frames.

These are core-only measurements. Input setup, checkpoint export/hashing,
WebRTC, product pacing, canvas presentation and audio playback are outside the
timing window. Audio is drained each frame. Checkpoint diagnostics can affect
caches and scheduling between calls; both variants receive the same diagnostics.
Reciprocal core throughput is **not measured product FPS**. No builds, profiling
or other emulator tests were run concurrently with these performance trials.
The non-isolated browser timer is quantized to approximately 0.1 ms; p95 values
pool the measured frames across the two seats and repeated trials. CPU frequency
and background OS scheduling were not experimentally controlled.

## Correctness evidence

- The differential interpolation harness extracts the original and modified
  classes from source and compiles them together to WASM. It passed
  **33,200,000 exact comparisons** covering X/Y interpolation, Z/W depth modes,
  zero spans and denominators, equal and unequal endpoints, signed attributes,
  linear and perspective paths, and pixels rejected before attribute setup.
  Depth mode and endpoint patterns vary independently. This is a bounded test,
  not an exhaustive proof over every possible 32-bit value.
- The initial eight-trial screen passed all **96 sampled output records**;
  the four-trial SIMD screen passed all **48**. Each record includes SHA-256 of
  the entire exported checkpoint, visible pixels, sampled audio, and both saves,
  plus RAM hash and frame. Every candidate matches the pinned core for the same
  seat with no excluded checkpoint bytes. Opposite seats agree on RAM/frame/saves;
  presentation state can legitimately differ between the two views.
- Full-build confirmation passed all 180 sampled output records as above.
- Six existing dispatcher/comparator tests passed, including all 4,096 ARM
  and 1,024 Thumb opcode mappings and strict state-comparison rejection cases.
- The full SIMD build passed the existing 600-frame-per-view Node recovery
  proof against pinned bytes: forced heap growth, clean over-limit allocation
  failure, scheduler jitter, checkpoint reload at step 360, and subsequent
  checkpoint/video/audio/save/RAM agreement for both views.
- The same recovery trace passed in **Chromium 149.0.7827.55 and Firefox 151.0**,
  for both visible-player seats. All five checkpoint/video/audio/save/RAM
  samples per view match Node, which matches pinned output. These are correctness
  checks; Firefox performance was not benchmarked.

Sampling occurs every 120 frames. These tests do not assert full state or output
equality at every intervening frame. They cover one game and one race fixture;
they do not establish universal NDS compatibility or production battle testing.
There was no two-device WAN/product E2E, mobile-device test or sustained soak in
this pass. The SIMD option remains disabled by default and nothing was deployed.

## Why the 16.72 ms budget still matters

The default core runs two complete DS machines in one worker. Each DS has ARM9
and ARM7 execution, software rendering, memory/peripheral work and local-radio
scheduling. This work runs even on a zero-latency link. The prior cooperative
profile attributed roughly 22% of samples to ARM dispatch and 16% to polygon
rasterization; inlining prevents treating those percentages as complete CPU or
renderer costs. Roughly 20% attributed to `md_run_frame` is not proof of wrapper
overhead.

At the longer-test baseline of 17.81 ms, reaching 16.72 ms requires about **6.1%
less core time**, before reserving time for other product work. A 0.5% saving
would be useful only as part of a larger improvement. Netcode can reduce waits
on lossy links, but cannot erase this local emulation cost.

The next useful core work is finer profiling of ARM instruction handlers,
memory access and branch/fetch helpers, followed by measured specialization of
the hottest paths. More integer SIMD is also testable, but neither a generic
SIMD flag nor another dispatcher switch is an established large gain. O3/LTO,
SIMD and existing memory/dispatch fast paths are already enabled.

An ARM-to-WASM block recompiler is a larger research direction, requiring code
invalidation, self-modifying-code handling, precise cycle/interrupt behavior
and checkpoint validation. It is not a switch that enables melonDS's native
JIT in a browser, and no speedup from such a recompiler is claimed here.

## Reproduction and artifacts

The host is an Intel i5-9400F with six physical cores and 16 GB RAM, running
Linux x86-64, Node 24.17.0, Chromium 149 and Emscripten 3.1.74. This does not
establish mobile ARM performance, device thermals or WebKit performance.

Build the control and quick variants:

```sh
/home/daudau/Code/emsdk/upstream/emscripten/emcmake cmake \
  -S rebit/emscripten -B build/speed -DCMAKE_BUILD_TYPE=Release \
  -DREBIT_MELONDS_DUAL_COOPERATIVE=ON -DREBIT_MELONDS_DUAL_BUILD_CLI=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/speed -j4
python research/build-variants.py
python research/test-interpolator.py
node research/benchmark.mjs
```

Quick variants recompile only the changed translation units into copies of the
control archive, then relink. Exact command arrays are recorded with each
artifact. A full candidate build avoids relying only on that quick workflow:

```sh
/home/daudau/Code/emsdk/upstream/emscripten/emcmake cmake \
  -S rebit/emscripten -B build/simd-full -DCMAKE_BUILD_TYPE=Release \
  -DREBIT_MELONDS_DUAL_COOPERATIVE=ON -DREBIT_MELONDS_DUAL_BUILD_CLI=OFF \
  -DREBIT_MELONDS_SIMD_ATTRIBUTES=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/simd-full -j4
python research/record-full-build.py
NDS_SPEED_OUTPUT=confirmation NDS_SPEED_FRAMES=1800 \
  NDS_SPEED_MATRIX='[["pinned","a"],["simd-full","a"],["simd-full","b"],["pinned","b"],["pinned","c"],["simd-full","c"]]' \
  node research/benchmark.mjs
python research/summarize.py
python research/validate-candidate.py > build/research/validation.log 2>&1
```

The Node proof needs explicit `.cjs` copies of the generated loaders because the
client package treats `.js` as ESM. The validation runner copies the exact bytes;
their SHA-256 hashes remain equal to the originals. The first attempt failed at
module loading, before emulation, and is retained in
[validation-loader-failure.log](build/research/validation-loader-failure.log).

Local artifacts:

- [Environment, manifest and fixture hashes](build/research/environment.json)
- [Candidate hashes](build/research/artifacts.json)
- [Source patch](build/research/source.patch)
- [Initial trial report](build/research/browser/report.json)
- [Short SIMD trial report](build/research/simd-browser/report.json)
- [Full-build confirmation](build/research/confirmation/report.json)
- [Aggregated timing summary](build/research/summary.json)
- [Final artifact/result audit](build/research/audit.json)
- [Arithmetic test log](build/research/interpolator-final.log)
- [Recovery proof](build/research/recovery-proof.json)
- [Browser recovery proof](build/research/browser-recovery-proof.json)
- [Validation log](build/research/validation.log)
- [Arithmetic harness](research/test-interpolator.py)
- [Browser timing/output harness](research/benchmark.mjs)
- [Recovery/browser validation runner](research/validate-candidate.py)

The pinned WASM SHA-256 is
`ae2c27ee9f2fac43d3d1e20f102b4a001f7797c9bb9042005fcf6d269bf0e9a3`
(1,447,300 bytes). The full SIMD CMake build is
`ead756fdbbadd47229133a7c8ba1f13b44cfde5cb3b980fc8519d422a80ce39e`
(1,447,048 bytes). Experimental artifacts retain the local build/ABI strings,
but are distinct binaries and must not be substituted into a live room or
published under an existing immutable release filename.
