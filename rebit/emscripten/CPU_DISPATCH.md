# Compact WASM CPU dispatch

The CPU portion of melonDS rebit `f0fd0ae8eaa6ac144c42fd0c78fb47e0d6851335`
is now represented in dual. Direct ARM9/ARM7 memory, cycle and branch helpers
were **already present**, as were direct ARM/Thumb handler calls, introduced by
dual commit `247d57ac`. This port replaces the older large opcode switch with
the donor's compact opcode-to-handler-ID table and dense direct-call switch.
It does not add a JIT, alter instruction semantics, or change rendering/netcode.

The header is generated from **dual's own** `src/ARM_InstrTable.h`, preserving
4096 ARM and 1024 Thumb mappings. Its generated code matches the donor after
accounting for dual's namespace and formatting. The direct helper block in
`src/ARM.h` already matches the donor exactly. Native dispatch remains unchanged;
the generated header is only included under `__EMSCRIPTEN__`.

## Reproduce

```sh
python3 rebit/emscripten/generate_arm_dispatch.py --check
node --test rebit/emscripten/tests/arm-dispatch.test.cjs \
  rebit/emscripten/tests/state-difference.test.cjs
emcmake cmake -S rebit/emscripten -B build/wasm-deterministic-compact-dispatch \
  -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release \
  -DREBIT_MELONDS_ROLLBACK=ON -DREBIT_MELONDS_DETERMINISTIC=ON
cmake --build build/wasm-deterministic-compact-dispatch --parallel 4
```

Keep a pre-change reference artifact; do not rebuild it with the modified source
and call that the baseline. The tests use locally authorized ROM/checkpoint files.

```sh
NDS_BASELINE_JS=/absolute/path/old/melonds_dual_rollback.js \
NDS_CANDIDATE_JS=/absolute/path/new/melonds_dual_rollback.js \
MKDS_ROM=/absolute/path/Mario-Kart-DS-USA.nds \
MKDS_CHECKPOINT=/absolute/path/race-baseline.bin \
NDS_CPU_OUTPUT=/absolute/path/cpu-comparison.json \
node rebit/emscripten/tests/compare-cpu-dispatch.cjs
```

This runs two linked consoles per runtime, with identical indexed inputs, 120
warm-up frames and 600 measured frames per repetition. Three baseline/candidate
pairs run sequentially in alternating order. Each module imports the checkpoint
once, then restores a reserved full rollback slot before each repetition. All
serialized console/radio/runtime parts and both framebuffers are SHA-256 compared
every 60 frames, including warm-up, without byte exclusions.

An initial benchmark that repeatedly imported the external checkpoint matched
the first baseline/candidate pair but failed repeated-run serialized console
equality (radio and video hashes matched). That failed result is retained as
`cpu-comparison-checkpoint-reimport-failure.json`. External checkpoint re-import
is not used as an exact reset oracle; the benchmark uses the rollback API being
validated. This does not establish repeated external-checkpoint import fidelity.

Timing covers only `md_run_frame`, including the two consoles' existing software
rendering and local scheduling. It excludes snapshots, hashing, input setup,
browser presentation and WebRTC; it is **not browser/player FPS**. Corrected
rollback is tested separately with `tests/proof-determinism.cjs` as described in
[DETERMINISM.md](DETERMINISM.md).

Neither the existing private playground's pinned artifacts nor product/CDN
artifacts are replaced by this isolated build. The build/ABI stays compatible;
the different artifact hash must still be pinned and agreed by all peers before
any rollout. Existing conservative rendering settings remain in place.

## Local results — 2026-09-09

The Emscripten 3.1.74 Release/O3/LTO/SIMD build passed; baseline and candidate
core compiler flags match. Six ROM-free tests passed, including exhaustive
opcode mappings and strict state-comparator tests.

The three paired Mario Kart race runs passed all 60 cross-run full-state/video
comparisons, without ignored bytes. Across 1800 measured frames per build:

- Baseline: median **21.519 ms**, mean 21.527 ms per two-console frame.
- Compact dispatch: median **21.506 ms**, mean 21.520 ms.

This is **no measurable performance improvement** in this workload. Aggregate
core-only throughput was 46.45 versus 46.47 frames/s, not browser/player FPS.
The comparison is against dual's existing direct-call interpreter, not against
the older virtual-call/function-pointer CPU implementation.

Baseline WASM SHA-256:
`0ab5649426539be5bb9cd5b79291ddb462e2e4906748dfad178adc34e4f52bcd`
(1,472,733 bytes).

Candidate WASM SHA-256:
`e3232325562097fe53b476eb68cc73b5206791efb7ef377e2e4f1422b1cb2d0f`
(1,474,837 bytes). The smaller generated C++ source does not imply a smaller
WASM binary; the candidate is 2104 bytes larger.

Raw A/B samples and full-state/video hashes are retained in
`build/wasm-deterministic-compact-dispatch/cpu-comparison.json`.

The candidate also passed the existing 600-frame Mario Kart rollback race proof
(frames 7612–8212): 116 strict state comparisons, 38 wrong-branch restores,
600 corrected replay frames, matching per-frame video/audio/save outputs, and
both corruption negative controls. No serialized bytes were ignored. Final
full-state hashes additionally match the previously recorded baseline race
proof. Results: `build/wasm-deterministic-compact-dispatch/rollback-race.json`.
This remains same-machine Linux x64 Node validation, not cross-browser/device
or universal game determinism. The existing native scheduler regression also
passed; native CPU dispatch was not modified.
