# Cooperative NDS WASM CPU specialization — 2026-10-10

The earlier roughly 17.7 ms result was an observed performance level, **not an
established ceiling**. This follow-up implements CPU specialization and observes
a materially larger gain than the previous renderer experiments. Nothing has
been deployed and experimental CMake options remain off by default.

## Initial result

Two repeated Chromium trials per variant, with two simultaneous replicas (each
running both DS consoles), gave:

| Variant | Mean dual-console core time | p95 | Trial pair means |
| --- | ---: | ---: | --- |
| Matched control | 17.721 ms | 19.5 ms | 17.707 / 17.735 ms |
| Full CPU specialization, inlined | 16.807 ms | 18.5 ms | 16.767 / 16.848 ms |
| Full CPU specialization, separate dispatchers | 17.329 ms | 19.1 ms | 17.273 / 17.384 ms |

The inlined candidate reduced mean core time by **5.16%**, about **0.91 ms per
dual-console frame**. Separate dispatchers saved about 2.21%. The inlined version
is selected for longer confirmation. These comparisons use the same FIFO
initialization fix in the control and candidates, explained below.

The longer full-build comparison confirms the CPU saving:

| Variant | Mean dual-console core time | p95 | Trial pair means |
| --- | ---: | ---: | --- |
| Matched control | 17.765 ms | 19.4 ms | 17.761 / 17.769 ms |
| Full CMake CPU build | 16.764 ms | 18.4 ms | 16.748 / 16.780 ms |
| CPU plus SIMD attributes | 16.729 ms | 18.3 ms | 16.706 / 16.752 ms |

Each trial ran 1,800 frames, with 120 warmup and 1,680 measured frames per seat.
Order was `control/CPU/CPU+SIMD/CPU+SIMD/CPU/control`. CPU specialization saved
**1.001 ms, or 5.63% of core time**. The full CMake WASM is byte-identical to the
inlined candidate from the shorter screen. All **180 sampled output records**
match the complete same-seat control output; no checkpoint bytes are excluded.

SIMD adds only **0.035 ms (0.21%)** compared with CPU specialization in this
sequence. That small additional effect does not establish a broadly useful
combined improvement. The **CPU-only change is the selected result** for recovery
and browser validation. Its performance gain repeated in both the short and
long workloads; the current evidence still covers one game and one host.

The selected CPU-only build also passed Node recovery checks, matching recovery
traces in Chromium and Firefox, and a separate Chromium cold-boot trace.

This remains core-call time, not measured product FPS. The approximately 16.72 ms
DS frame budget also needs room for product overhead, and the measured p95 is
still above that budget. This evidence does not establish sustained full-speed
netplay, a universal browser/device speedup or production battle testing.

## Implementation

The generic interpreter uses one set of handlers for both ARM9 and ARM7. Existing
fast helpers choose a CPU type at runtime for memory accesses, cycle accounting
and branches; many handlers also check the CPU number directly.

[generate-specialized-cpu.py](research/generate-specialized-cpu.py) generates
typed ARM9 and ARM7 copies from the existing ALU, load/store, branch and system
instruction sources. It replaces CPU-number reads with the known constant and
qualifies calls to the same CPU-specific methods already selected by the
generic helper. Both ARM and Thumb mappings come from the canonical instruction
table. The existing special ARM9 BLX-immediate path remains unchanged.

This allows compile-time removal of CPU-type selection throughout the handler
paths. It preserves instruction algorithms, memory permissions and access order,
cycles, interrupts, pipeline behavior, radio scheduling and rendering. There is
no guest-code cache, prediction, input-delay change or new serialized CPU state.
This is ahead-of-time specialization of the interpreter, not a guest-code JIT.

The generated source lives under the build directory. The checked-in experiment
consists of a generator, CMake integration and four guarded dispatch call-site
changes in [ARM.cpp](src/ARM.cpp). It extends beyond the old partial ARM9 ALU
experiment: all four handler families and both guest CPU types are specialized.

### Compiler layout was measured

Final WASM inspection shows that normal optimization folds the typed dispatchers
into `md_run_frame`, increasing that function's body from **8,923 to 205,125
bytes**. A build-local Binaryen adapter, combined with C++ no-inline attributes,
keeps all four dispatchers separate and restores the 8,923-byte frame function.
The adapter runs its named no-inline passes **before** normal optimization and
verifies that all four named functions exist. It does not modify the SDK.

Despite the larger frame function, the inlined build is faster in this test.
Function size alone was not used to choose a winner. The outlined candidate is
retained as a rejected alternative for this workload. Inspection records are
available for [control](build/research/cpu-control/function-sizes.json),
[inlined](build/research/cpu-inline/function-sizes.json), and
[outlined](build/research/cpu-outline/function-sizes.json) builds.

## A checkpoint determinism bug found during validation

The first CPU-specialized build matched RAM, pixels, audio and saves, but failed
the full external-checkpoint comparison. Import/export before execution matched
exactly. After execution, differences were confined to three unused bytes of
GPU command FIFO entries in the `GP3D` section. These bytes came from stack
storage and changed with compiler layout.

`GPU3D::CmdFIFOEntry` is an eight-byte union containing a four-byte parameter,
one-byte command and three reserved bytes. Its FIFO serializer writes the whole
entry. The source already clears `_contents` before filling the entry in two
locations, but **only when `REBIT_MELONDS_ROLLBACK` is defined**. The default
cooperative profile does not define it.

`REBIT_MELONDS_ZERO_GPU_FIFO_PADDING` applies that existing initialization to
the cooperative experiment too. It changes neither the struct size nor the
checkpoint format. Existing checkpoints still import. Newly written entries
have zero reserved bytes rather than arbitrary stack contents.

The control and optimization candidates both include the fix. **No bytes are
excluded from their full-checkpoint comparisons.** Separately, the matched
control was compared with pinned production bytes at steps 0, 1 and 120:

- Import/export at step 0 was byte-identical.
- Every changed byte at steps 1 and 120 was required to be an identified FIFO
  or pipeline reserved byte, and its new value was required to be zero.
- Every other checkpoint byte and all sampled RAM/frame/pixels/audio/saves
  matched the pinned core.

This explicitly validates the intended initialization change; it is not an
exception permitting arbitrary state differences in the CPU optimization.
The default protocol's usual RAM-hash verification does not include these FIFO
padding bytes; this finding alone does not demonstrate a production gameplay
desync. It fixes full-checkpoint reproducibility needed for this strict proof.
The failed first attempt and raw diagnostic snapshots are preserved in
[the original failure](build/research/cpu-browser-uncanonicalized/failure.log),
[the CPU checkpoint diagnosis](build/research/cpu-checkpoint-diagnostic/report.json),
[the padding-control comparison](build/research/cpu-control-checkpoint-diagnostic/report.json)
and [its output checks](build/research/cpu-control-checkpoint-diagnostic/outputs.json).

## Validation and limitations

- A compiled WASM test passed **10,240 opcode/CPU routing checks**: all 4,096 ARM
  and 1,024 Thumb decoder entries for each of the two CPU types. Handlers in that
  test are recording stubs; it validates routing, not exhaustive ISA semantics.
- All **72 sampled output records** in the initial six browser trials matched
  the same-seat control's complete checkpoint, visible pixels, sampled audio,
  saves, RAM hash and frame. Opposite views agreed on RAM/frame/saves. Samples
  are taken every 120 frames, not at every intervening instruction or frame.
- The longer six-trial matrix passed all 180 sampled output records. The full
  CPU-only CMake build reproduced the screened WASM hash exactly.
- The selected CPU-only build passed the existing Node recovery proof against
  the matched control: 600 frames per view, forced heap growth, clean failure of
  an over-limit allocation, scheduler jitter, checkpoint reload at step 360,
  and complete sampled checkpoint/video/audio/save/RAM agreement afterwards.
- Recovery traces passed in **Chromium 149.0.7827.55 and Firefox 151.0**, for both
  visible-player seats. Complete checkpoint/video/audio/save/RAM samples match
  the Node candidate, which matches the same-seat control. This verifies
  correctness in Firefox; Firefox speed was not benchmarked.
- A separate **720-frame cold-boot trace** in Chromium passed all 24 sampled
  output records across control/candidate and both views, starting without an
  imported checkpoint. This exercises startup code but does not validate a
  complete product lobby/join flow or new multiplayer association.
- Six existing canonical dispatcher/state-comparator tests passed. All 11
  regenerated typed C++/header files match the sources used by the full build.
  The final [artifact/result audit](build/research/cpu-audit.json) passed.

The fixture is the existing Mario Kart DS race checkpoint at frame 7612. Inputs
are deterministic and indexed by frame. Each first-screen trial runs 720 frames;
the first 120 are excluded from timings. Order:
`control/inlined/outlined/outlined/inlined/control`. Hashing, checkpoint export,
input setup, network, canvas presentation, audio playback and product pacing
are outside the timed `_md_run_frame` call. Both replicas run on one workstation;
they are not independent repetitions. No builds, profiles or other emulator
tests overlap performance measurements.

Host: Intel i5-9400F, six physical cores, 16 GB RAM, Linux x86-64, Chromium 149,
Node 24.17.0, Emscripten 3.1.74. Browser timers have roughly 0.1 ms quantization;
CPU frequency and OS background scheduling are not controlled. These are a small
number of paired trials, with no statistical confidence interval claimed.
There was no physical mobile ARM test, other-game matrix, WAN/product E2E or
long-duration soak. The measured improvement is sufficient to justify further
work on this candidate; it is not a production release gate or a 60-FPS promise.

## Reproduction

Build the selected CPU-only candidate from all sources:

```sh
/home/daudau/Code/emsdk/upstream/emscripten/emcmake cmake \
  -S rebit/emscripten -B build/cpu-final -DCMAKE_BUILD_TYPE=Release \
  -DREBIT_MELONDS_DUAL_COOPERATIVE=ON -DREBIT_MELONDS_DUAL_BUILD_CLI=OFF \
  -DREBIT_MELONDS_SPECIALIZED_CPU=ON -DREBIT_MELONDS_ZERO_GPU_FIFO_PADDING=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/cpu-final -j4
python research/record-full-build.py cpu-final
python research/test-specialized-dispatch.py
```

CPU specialization requires the FIFO initialization option. CMake now rejects
that combination when initialization is disabled, as well as combinations with
the separate indirect-dispatch experiment.

The short-screen variants use copied archives from `build/speed` and the first
full specialized build `build/cpu-specialized`; they recompile the FIFO source
and, for the outlined variant, both dispatcher translation units. Exact command
arrays and environment overrides are recorded in each artifact directory.

```sh
python research/build-cpu-variants.py cpu-control cpu-inline cpu-outline
NDS_SPEED_OUTPUT=cpu-browser NDS_SPEED_REFERENCE=cpu-control \
  NDS_SPEED_MATRIX='[["cpu-control","a"],["cpu-inline","a"],["cpu-outline","a"],["cpu-outline","b"],["cpu-inline","b"],["cpu-control","b"]]' \
  node research/benchmark.mjs
python research/summarize.py
```

Longer comparison and recovery checks:

```sh
python research/build-cpu-variants.py cpu-simd
NDS_SPEED_OUTPUT=cpu-confirmation NDS_SPEED_FRAMES=1800 \
  NDS_SPEED_REFERENCE=cpu-control \
  NDS_SPEED_MATRIX='[["cpu-control","a"],["cpu-final","a"],["cpu-simd","a"],["cpu-simd","b"],["cpu-final","b"],["cpu-control","b"]]' \
  node research/benchmark.mjs
python research/validate-candidate.py cpu-final cpu-control
NDS_SPEED_OUTPUT=cpu-boot NDS_SPEED_BOOT=1 NDS_SPEED_REFERENCE=cpu-control \
  NDS_SPEED_MATRIX='[["cpu-control","boot"],["cpu-final","boot"]]' \
  node research/benchmark.mjs
```

Evidence: [artifact hashes](build/research/artifacts.json),
[browser trials](build/research/cpu-browser/report.json),
[timing summary](build/research/cpu-browser/summary.json),
[longer trials](build/research/cpu-confirmation/report.json),
[longer summary](build/research/cpu-confirmation/summary.json),
[Node recovery](build/research/cpu-final-validation/recovery-proof.json),
[browser recovery](build/research/cpu-final-validation/browser-recovery-proof.json),
[cold-boot trace](build/research/cpu-boot/report.json),
[compiled dispatcher test](research/test-specialized-dispatch.py),
[checkpoint diagnostic](research/diagnose-cpu-checkpoint.cjs),
[timing harness](research/benchmark.mjs),
[full source generator](research/generate-specialized-cpu.py).

Selected CPU-only WASM: **1,500,374 bytes**, SHA-256
`505aee252d5ba24116ee33d47c26c5cd6108f81a716c02f00f38a3a443deffd0`.
Matched control: **1,447,373 bytes**, SHA-256
`3f113a5c8f4741d086d89efad5d430eb051ee8b5eb8b480cac22a3edd8b11771`.
The candidate is approximately 3.7% larger before compression. The complete
tracked source change is retained in [cpu-source.patch](build/research/cpu-source.patch).

The experiment is based on `0cb05783dc3d9c9032aff0c85ec6fc944c808663` in isolated
branch `experiment/cooperative-core-speed-20261010`. Production/client files,
core manifests and CDN artifacts remain unchanged. Experimental artifacts keep
local build/ABI strings for testing and must not be substituted into an existing
live release. A production change would require its own artifact identity and
product/device validation.
