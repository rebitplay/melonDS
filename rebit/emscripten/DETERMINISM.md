# Isolated deterministic reference

This is an experimental **N-console runtime on each physical machine**, not N
networked emulator processes exchanging DS packets over the internet. Every
replica simulates the entire local wireless system; a future client transports
frame-indexed inputs. The working one-console-per-browser preview is unchanged.

`REBIT_MELONDS_DETERMINISTIC=ON` selects build `melonds-dual-deterministic-1`, ABI
`rebit-melonds-deterministic-v1`. It requires rollback, excludes the existing
playground profile, and is OFF by default. Existing client pins are not changed;
this is not a deployed netcode or performance release.

## Contract and implementation

- Identical ROM, core bytes, per-seat initial saves, seed, BIOS/firmware/settings
  and indexed button/touch inputs are required on every replica. The existing
  runtime fixes RTC and derives console MACs from seed plus console index.
- The interpreter and device events advance in emulated time. The existing
  fixed radio-turn scheduler, including scheduled frame completion, is retained.
  Host watchdogs fail the rollback runtime closed; a timeout is not accepted as
  a successful speculative frame. CPU worker timing is deliberately perturbed
  in the tests, not treated as an input to the emulated radio.
- All consoles render, regardless of which player the local UI displays.
  Software rasterization is synchronous inside each console worker. Four
  preallocated WASM workers support up to four consoles without requiring extra
  renderer workers.
- The software renderer always rasterizes rather than taking its identical-frame
  cache shortcut. A restored cold texture cache and a warm forward-only cache
  therefore use the same path. The byte oracle has **no cache-byte exemptions**.
- Every console generates audio. Each emulated SPU is drained at the same frame
  boundary into a presentation-only copy. Playback reads, chunk sizes and the
  visible player cannot change serialized SPU state. `md_audio_read_player`
  exposes per-console audio for tests; `md_audio_read` still selects the local
  player. Consume after each `md_run_frame`: only the latest frame is retained,
  bounded to 4096 stereo samples per console. Overflow faults the reference;
  rollback clears presentation copies and replay regenerates them.
- A view change no longer invalidates deterministic-profile rollback slots.
  It is not a change to the simulated system.
- The radio snapshot includes a new `RTIM` section **before** `LMP.`: pending
  inputs, reply sequence and per-console packet/reply/scheduling context are
  now included in the byte comparison. These were already restored from
  out-of-band ring fields, but formerly invisible to the parts-only oracle.
- Ordinary rollback lifecycle guards, frame tags, bounded memory and sticky
  recovery faults remain. Only joint completed-frame boundaries are accepted.

The baseline old-profile test failed full-state comparison at frame 0, in the
`RSFT` software-renderer section, with opposite local views. That is a real
serialized-state difference, **not proof that the guest CPU/game already
desynchronized**. Presentation-only differences can be safe; this deliberately
conservative profile removes them to establish a stronger reference before
reintroducing optimizations under differential tests.

## Reproduce

Use the installed Emscripten 3.1.74 SDK and an isolated output directory:

```sh
emcmake cmake -S rebit/emscripten -B build/wasm-deterministic \
  -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release \
  -DREBIT_MELONDS_ROLLBACK=ON -DREBIT_MELONDS_DETERMINISTIC=ON
cmake --build build/wasm-deterministic --parallel 4
node --test rebit/emscripten/tests/state-difference.test.cjs
```

Run the real-ROM oracle from this repository. All paths below must refer to
authorized local files; nothing downloads or changes ROMs, saves or traces.

```sh
MELONDS_DUAL_JS=/absolute/path/build/wasm-deterministic/dist/melonds_dual_rollback.js \
MELONDS_DUAL_WASM=/absolute/path/build/wasm-deterministic/dist/melonds_dual_rollback.wasm \
MKDS_ROM=/absolute/path/Mario-Kart-DS-USA.nds \
NDS_DETERMINISM_TIMELINE=/absolute/path/confirmed-input-trace.json \
NDS_DETERMINISM_FRAMES=4500 NDS_DETERMINISM_WINDOW=4 \
NDS_DETERMINISM_OUTPUT=/absolute/path/result.json \
node rebit/emscripten/tests/proof-determinism.cjs
```

The trace uses the existing development trace format: `seed`, `confirmed`, and
`inputs[frame][console]`. Only confirmed inputs are consumed; historical rollback
operations/hashes are not trusted as the reference. New wrong branches are
generated locally. Two separately initialized WASM modules with separate memory
simulate the same system; one advances forward-only, the other repeatedly
executes wrong remote inputs, corrupts disposable in-memory cartridge save bytes,
restores and replays. Views start on opposite players and change during the test;
audio reads use different chunk sizes. Immediate restore and corrected simulation
must match byte-for-byte for all serialized console/radio/runtime parts, and
every replayed frame's screens, audio and cartridge save hashes must match.
Negative controls require detection of guest RAM and pending-input corruption.

For an active-race checkpoint test, replace `NDS_DETERMINISM_TIMELINE` with
`MKDS_CHECKPOINT`, set `NDS_DETERMINISM_FRAMES=600` and
`NDS_DETERMINISM_WINDOW=16`. This exercises a locally supplied checkpoint and
does **not** independently validate startup. For synthetic cold-boot tests omit
both and set `NDS_DETERMINISM_PLAYERS=2`, `3`, or `4`. Those larger-console tests
are not evidence of connected 3/4-player gameplay. Tests support windows 1–16;
that does not change client prediction/delay limits.

To reproduce the expected old-profile full-state failure, point at the isolated
version-3 baseline binary and explicitly set
`NDS_DETERMINISM_EXPECT_BUILD=melonds-dual-rollback-experimental-3`. This is a
negative control, not a relaxed success gate. The old version-3 oracle remains
separate and keeps its original build pin and assertions.

The native ROM-free `nds_rollback_scheduler_test` covers all turn owners with
2/3/4 consoles, reverse completion-thread launch order, and a host waiting for
replies while guests finish. Build it with rollback enabled and run CTest.

## What this does not prove

This establishes bounded repeatability for the tested software WASM profile,
not universal melonDS determinism. Serialized equality cannot prove that no
future-affecting field is omitted. More games, longer traces, reset/peripheral
transitions, different browsers and physical devices still need testing with
the exact same core bytes. Real microphone/camera/WFC or other external inputs
must be synchronized or consistently excluded. No JIT, GPU renderer, native/WASM
cross-build equivalence, production input transport, or 60 FPS guarantee is
established here. The production `md_state_hash` remains RAM/frame-only; the new
test uses SHA-256 and strict snapshot comparisons instead.

Before production, validate connected 3/4-player gameplay, target-browser/device
repeatability, lifecycle/desync recovery and snapshot/replay headroom. Optimize
only after those gates, preserving this profile as the correctness reference.

## Verified locally — 2026-09-09

Final WASM SHA-256:
`0ab5649426539be5bb9cd5b79291ddb462e2e4906748dfad178adc34e4f52bcd`.
The following runs all used those exact bytes, Node v24.17.0 on Linux x64, Mario
Kart DS USA, and strict runtime metadata coverage with no ignored snapshot bytes:

| Workload | State comparisons | Wrong-branch restores | Outcome |
| --- | ---: | ---: | --- |
| Two-console confirmed-input cold boot through frame 4500, four-frame branches | 3377 | 1125 | Passed |
| Two-console local race checkpoint, frames 7612–8212, branches up to 16 frames | 116 | 38 | Passed |
| Two-console synthetic boot, 300 frames | 227 | 75 | Passed |
| Three-console synthetic boot, 300 frames | 227 | 75 | Passed |
| Four-console synthetic boot, 300 frames | 227 | 75 | Passed |

All five runs also detected deliberate RAM/input-metadata corruption and restored
the reference afterward. The timeline/race runs had real emulated host commands
and guest replies; the synthetic boot runs had **zero radio traffic**. Thus the
3/4-console result must not be presented as connected multiplayer validation.
Snapshots contained 18,274,788 / 27,346,481 / 36,418,174 bytes for 2/3/4 consoles.

The native scheduling regression passed for 2/3/4 consoles (0.92 seconds). Three
ROM-free comparator/section-coverage tests passed. Rebuilding the old profile
with the deterministic option OFF and running its unchanged version-3 oracle
passed 40 replay checks plus its six lifecycle checks. Its rebuilt bytes changed;
no published artifact was replaced and the live software preview remained on
its original single-console WASM hash
`20b4cc8d0f732a32b2fee3b769d03de01f743d30e65599c3a76e1882218545ee`.

Raw results are in the sibling client's ignored `artifacts/` directory:
`nds-deterministic-final-timeline-2.json`, `nds-deterministic-final-race-2.json`,
`nds-deterministic-final-boot-{2,3,4}.json`, and
`nds-determinism-default-regression.json`. Earlier intermediate runs are not the
final artifact gate. The baseline negative control is
`nds-determinism-view-baseline.json`.

Conclusion: no inherent impossibility was found. This opt-in profile reproduces
the tested local multi-console workloads, including corrected rollback. Remaining
production blockers are validation breadth, optimized-renderer compatibility and
performance—not a demonstrated unavoidable source of randomness in the DS CPU.
