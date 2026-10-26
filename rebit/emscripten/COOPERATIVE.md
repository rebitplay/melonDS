# Cooperative WebView runtime

`melonds-dual-cooperative-5` (ABI `rebit-melonds-dual-cooperative-3`) adds
per-seat cartridges for cross-version play such as Pokemon Diamond with Pearl.
`md_stage_rom(player, length)` returns a core-owned buffer to fill and
`md_load_staged(players, seed)` boots each seat from its own buffer; a seat left
unstaged runs seat 0's ROM. Each staged buffer is released once its cartridge
has been parsed, so the heap peak for two different games matches the older
single-ROM path, which kept the caller's ROM allocation for the whole session.
`md_load()` is unchanged. `tests/proof-cross-rom.cjs` proves the staged and legacy
paths reproduce cooperative-4 exactly with one ROM, and that two different
cartridges stay deterministic and recover from a checkpoint.


`melonds-dual-cooperative-3` runs both DS consoles in a single WASM worker. It
includes the local wireless reply barrier from `21abbb21`: a host cannot complete
a command until every connected local client has processed it and replied.

## Memory

The cooperative build starts at 128 MiB and may grow to 1 GiB. Threaded and
rollback builds retain their fixed heaps. The runtime ABI remains
`rebit-melonds-dual-cooperative-2`; changing heap capacity does not change the C
API or checkpoint format. JavaScript callers must reacquire `HEAP*` views after
allocating, loading, running a frame or exporting/importing a checkpoint.

The browser worker retains its ROM allocation for checkpoint recovery. Cartridge
size and temporary serialization buffers determine the high-water heap capacity;
128 MiB is an initial allocation, not a limit or a claim about resident memory.

## Regression proof

`tests/proof-cooperative-memory.cjs` compares a clean fixed-memory `21abbb21`
build and the candidate, serially, using the authorized Mario Kart race fixture:

```
node rebit/emscripten/tests/proof-cooperative-memory.cjs \
  reference.js reference.wasm candidate.js candidate.wasm \
  Mario-Kart-DS-USA.nds race.bin result.json
```

It checks 600 frames per visible seat, full public checkpoint bytes, video,
audio, both cartridge saves, forced heap growth, over-limit allocation failure,
and destroy/load/import recovery. Opposite views must agree on frame/RAM/save
traces. The companion client `proof-nds-cooperative-browsers.mjs` runs this same
workload in Chromium, Firefox and WebKit and compares against the Node traces.

These bounded proofs do not turn the periodic 32-bit RAM checksum into a complete
simulation-state oracle. Renderer presentation state differs between visible
seats, display capture remains guest-visible, and real phone performance still
requires device testing. Keep the threaded determinism reference separate.
