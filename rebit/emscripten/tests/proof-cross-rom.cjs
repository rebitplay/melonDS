// Differential release proof for per-seat cartridges (md_stage_rom/md_load_staged).
// Usage: node proof-cross-rom.cjs reference.js reference.wasm candidate.js candidate.wasm rom-a.nds rom-b.nds rom-c.nds
//   rom-a/rom-b: two wireless-compatible dumps with different bytes (e.g. one byte patched)
//   rom-c:       a different game, to prove that seat 2 really boots its own cartridge
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { createHash } = require('node:crypto');
const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');

const [referenceJs, referenceWasm, candidateJs, candidateWasm, romAPath, romBPath, romCPath] = process.argv.slice(2);
if (!romCPath) throw new Error('Usage: node proof-cross-rom.cjs reference.js reference.wasm candidate.js candidate.wasm rom-a rom-b rom-c');
const romA = fs.readFileSync(romAPath);
const romB = fs.readFileSync(romBPath);
const romC = fs.readFileSync(romCPath);
assert.notEqual(hash(romA), hash(romB), 'rom-a and rom-b must differ');
const seed = 0x9abcdef012345678n;
const seedLow = Number(seed & 0xffffffffn);
const seedHigh = Number(seed >> 32n);

// melonDS keeps some process-wide state between loads, so a second boot in one
// module never matches a first boot (the released core behaves the same). The
// product starts every session in a new worker; every trace here does too.
async function open(js, wasm) {
    delete require.cache[require.resolve(js)];
    return require(js)({ wasmBinary: fs.readFileSync(wasm), print() {}, printErr() {} });
}

function error(core) {
    return core.UTF8ToString(core._md_last_error());
}

function loadLegacy(core, rom) {
    const pointer = core._malloc(rom.length);
    assert.ok(pointer);
    core.HEAPU8.set(rom, pointer);
    assert.equal(core._md_load(pointer, rom.length, 2, seedLow, seedHigh), 1, error(core));
    core._free(pointer);
}

function stage(core, player, rom) {
    const pointer = core._md_stage_rom(player, rom.length);
    assert.ok(pointer, error(core));
    // Staging may grow memory; always write through a fresh view.
    core.HEAPU8.set(rom, pointer);
}

function loadStaged(core, roms) {
    roms.forEach((rom, player) => rom && stage(core, player, rom));
    assert.equal(core._md_load_staged(2, seedLow, seedHigh), 1, error(core));
}

function bytes(core, pointer, length) {
    return core.HEAPU8.slice(pointer, pointer + length);
}

function checkpoint(core) {
    assert.equal(core._md_export_checkpoint(), 1, error(core));
    const state = bytes(core, core._md_checkpoint_data(), core._md_checkpoint_size());
    core._md_clear_checkpoint();
    return state;
}

function run(core, view, frames) {
    core._md_set_visible_player(view);
    const samples = [];
    for (let step = 1; step <= frames; step++) {
        for (let player = 0; player < 2; player++) {
            // Hold A on a slow cadence, plus Start bursts, so both menus advance.
            const pressed = ((step + player * 23) % 90 < 30 ? 1 : 0) | (step % 240 < 6 ? 8 : 0);
            core._md_set_input(player, 0x0fff ^ pressed, 0, 0, 0);
        }
        core._md_set_scheduler_jitter(step % 7);
        assert.equal(core._md_run_frame(), 1, error(core));
        assert.equal(core._md_frame(0), core._md_frame(1));
        const audioFrames = core._md_audio_read(4096);
        const audio = bytes(core, core._md_audio_buffer(), audioFrames * 4);
        if (step % 60 !== 0) continue;
        const saves = [0, 1].map((player) => hash(bytes(core, core._md_save_data(player), core._md_save_size(player))));
        samples.push({
            frame: core._md_frame(0),
            ram: core._md_state_hash() >>> 0,
            saveSizes: [core._md_save_size(0), core._md_save_size(1)],
            saves,
            pixels: hash(bytes(core, core._md_framebuffer(view), 256 * 384 * 4)),
            audio: hash(audio),
        });
    }
    return samples;
}

(async () => {
    const result = {};
    const reference = () => open(referenceJs, referenceWasm);
    const candidate = () => open(candidateJs, candidateWasm);
    {
        const core = await candidate();
        result.candidateBuild = core.UTF8ToString(core._md_build_id());
        result.candidateAbi = core.UTF8ToString(core._md_runtime_abi());
        assert.equal(core._md_api_version(), 1);
    }

    // 1. One shared ROM: the staged path and the legacy path both reproduce
    //    the released core exactly, for each visible seat.
    for (const view of [0, 1]) {
        let core = await reference();
        loadLegacy(core, romA);
        const expected = run(core, view, 900);
        const expectedState = hash(checkpoint(core));
        for (const [label, load] of [
            ['legacy md_load', (target) => loadLegacy(target, romA)],
            ['seat-1-only staging', (target) => loadStaged(target, [romA])],
            ['both-seat staging', (target) => loadStaged(target, [romA, romA])],
        ]) {
            core = await candidate();
            load(core);
            assert.deepEqual(run(core, view, 900), expected, `${label} diverged (view ${view})`);
            assert.equal(hash(checkpoint(core)), expectedState, `${label} checkpoint diverged (view ${view})`);
        }
    }
    result.sameRom = 'identical to the released core for both views, legacy and staged';

    // 2. Two different dumps of one game stay deterministic across views and
    //    survive the product's destroy/restage/import recovery.
    let core = await candidate();
    loadStaged(core, [romA, romB]);
    const crossView0 = run(core, 0, 900);
    const crossState = checkpoint(core);
    core._md_destroy();
    loadStaged(core, [romA, romB]);
    const pointer = core._malloc(crossState.length);
    core.HEAPU8.set(crossState, pointer);
    assert.equal(core._md_import_checkpoint(pointer, crossState.length), 1, error(core));
    core._free(pointer);
    assert.equal(core._md_state_hash() >>> 0, crossView0.at(-1).ram, 'cross-ROM recovery restored another state');
    assert.deepEqual(
        run(core, 0, 120).map(({ ram }) => ram).length,
        2,
    );
    core = await candidate();
    loadStaged(core, [romA, romB]);
    const crossView1 = run(core, 1, 900);
    assert.equal(hash(checkpoint(core)), hash(crossState), 'cross-ROM checkpoint depends on the visible seat');
    assert.deepEqual(
        crossView0.map(({ frame, ram, saves }) => ({ frame, ram, saves })),
        crossView1.map(({ frame, ram, saves }) => ({ frame, ram, saves })),
        'cross-ROM simulation depends on the visible seat',
    );
    core = await reference();
    loadLegacy(core, romA);
    const sameDump = run(core, 0, 900);
    result.crossDump = {
        frames: crossView0.at(-1).frame,
        ram: crossView0.at(-1).ram,
        differsFromSharedRom: crossView0.some((sample, index) => sample.ram !== sameDump[index].ram),
    };

    // 3. A different game in seat 2 really boots from its own cartridge.
    core = await candidate();
    loadStaged(core, [romA]);
    const sameGame = run(core, 1, 300);
    core = await candidate();
    loadStaged(core, [romA, romC]);
    const otherGame = run(core, 1, 300);
    assert.equal(otherGame.at(-1).saveSizes[0], sameGame.at(-1).saveSizes[0], 'seat 1 cartridge changed');
    assert.notEqual(otherGame.at(-1).saveSizes[1], sameGame.at(-1).saveSizes[1], 'seat 2 did not get its own cartridge');
    assert.notEqual(otherGame.at(-1).pixels, sameGame.at(-1).pixels, 'seat 2 shows the seat 1 game');
    result.otherGame = { saveSizes: otherGame.at(-1).saveSizes, heapBytes: core.HEAPU8.byteLength };

    // 4. Invalid staging fails cleanly and never leaks into the next load.
    core = await candidate();
    assert.equal(core._md_stage_rom(4, romA.length), 0);
    assert.equal(core._md_stage_rom(-1, romA.length), 0);
    assert.equal(core._md_stage_rom(0, 16), 0);
    assert.equal(core._md_load_staged(2, seedLow, seedHigh), 0, 'loaded without a seat 1 ROM');
    stage(core, 1, romB);
    assert.equal(core._md_load_staged(2, seedLow, seedHigh), 0, 'loaded with only a seat 2 ROM');
    stage(core, 0, romA);
    stage(core, 2, romC);
    assert.equal(core._md_load_staged(2, seedLow, seedHigh), 0, 'accepted a ROM for a seat outside the room');
    assert.equal(core._md_is_loaded(), 0);
    stage(core, 0, romA);
    assert.equal(core._md_load_staged(2, seedLow, seedHigh), 1, error(core));
    assert.equal(core._md_is_loaded(), 1);
    result.errors = 'rejected, consumed, and recoverable';

    console.log(JSON.stringify(result, null, 2));
})().catch((failure) => {
    console.error(failure);
    process.exit(1);
});
