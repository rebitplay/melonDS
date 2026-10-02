// Differential release proof for the cooperative WebView heap policy.
// Usage: node proof-cooperative-memory.cjs reference.js reference.wasm candidate.js candidate.wasm rom.nds race.bin result.json
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { createHash } = require('node:crypto');
const { execFileSync } = require('node:child_process');
const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');

async function trace(options) {
    const core = await require(options.js)({ wasmBinary: fs.readFileSync(options.wasm), print() {}, printErr() {} });
    const initialHeap = core.HEAPU8.byteLength;
    const rom = fs.readFileSync(options.rom);
    const checkpoint = fs.readFileSync(options.checkpoint);
    const copy = (bytes) => {
        const pointer = core._malloc(bytes.length);
        assert.ok(pointer);
        core.HEAPU8.set(bytes, pointer);
        return pointer;
    };
    const romPointer = copy(rom);
    const seed = 0x9abcdef012345678n;
    const load = () => {
        assert.equal(core._md_load(romPointer, rom.length, 2, Number(seed & 0xffffffffn), Number(seed >> 32n)), 1);
        core._md_set_visible_player(options.view);
    };
    load();
    const bootHeap = core.HEAPU8.byteLength;
    const pointer = copy(checkpoint);
    assert.equal(core._md_import_checkpoint(pointer, checkpoint.length), 1, core.UTF8ToString(core._md_last_error()));
    core._free(pointer);
    const importedHeap = core.HEAPU8.byteLength;
    if (options.grow) {
        const before = core.HEAPU8.buffer;
        const stateHash = core._md_state_hash() >>> 0;
        const scratch = core._malloc(importedHeap);
        assert.ok(scratch);
        assert.ok(core.HEAPU8.byteLength > importedHeap);
        assert.notEqual(core.HEAPU8.buffer, before);
        // Touch each page through the refreshed view; exercise an actual heap
        // allocation larger than a DS cartridge, then restore the race state.
        for (let offset = 0; offset < importedHeap; offset += 65536) core.HEAPU8[scratch + offset] = 0x5a;
        core._free(scratch);
        assert.equal(core._malloc(1073741824), 0, 'An over-limit allocation must fail cleanly.');
        assert.equal(core._md_state_hash() >>> 0, stateHash);
    }
    const samples = [];
    for (let step = 1; step <= 600; step++) {
        for (let player = 0; player < 2; player++) {
            const pressed = 1 | ((step + player * 23) % 90 < 30 ? 16 : 0);
            core._md_set_input(player, 0x0fff ^ pressed, 0, 0, 0);
        }
        core._md_set_scheduler_jitter(step % 7);
        assert.equal(core._md_run_frame(), 1);
        assert.equal(core._md_frame(0), core._md_frame(1));
        // Presentation reads must not affect the opposite replica's simulation.
        const audioFrames = core._md_audio_read(4096);
        const audio = core.HEAPU8.slice(core._md_audio_buffer(), core._md_audio_buffer() + audioFrames * 4);
        if (step % 120 !== 0) continue;
        assert.equal(core._md_export_checkpoint(), 1);
        const state = core.HEAPU8.slice(core._md_checkpoint_data(), core._md_checkpoint_data() + core._md_checkpoint_size());
        core._md_clear_checkpoint();
        const pixels = core._md_framebuffer(options.view);
        const saves = await Promise.all([0, 1].map((player) => {
            const length = core._md_save_size(player);
            const ptr = core._md_save_data(player);
            return hash(core.HEAPU8.subarray(ptr, ptr + length));
        }));
        samples.push({ frame: core._md_frame(0), ram: core._md_state_hash() >>> 0, state: await hash(state), saves,
            pixels: await hash(core.HEAPU8.subarray(pixels, pixels + 256 * 384 * 4)), audio: await hash(audio) });
        if (step === 360) {
            // Same lifecycle as the product's two-sided recovery. Retaining the
            // ROM and reacquiring heap views must survive destroy/load/import.
            load();
            const restore = copy(state);
            assert.equal(core._md_import_checkpoint(restore, state.length), 1);
            core._free(restore);
            assert.equal(core._md_state_hash() >>> 0, samples.at(-1).ram);
        }
    }
    const result = { build: core.UTF8ToString(core._md_build_id()), initialHeap, bootHeap, importedHeap,
        finalHeap: core.HEAPU8.byteLength, samples };
    core._md_destroy();
    core._free(romPointer);
    return result;
}

if (require.main !== module) {
    module.exports = { trace };
} else if (process.argv[2] === '--trace') {
    trace(JSON.parse(process.argv[3])).then((result) => process.stdout.write(JSON.stringify(result))).catch((error) => {
        console.error(error); process.exitCode = 1;
    });
} else {
    const [referenceJs, referenceWasm, js, wasm, rom, checkpoint, output] = process.argv.slice(2);
    assert.ok(output, 'Provide reference/candidate JS and WASM, ROM, checkpoint and output paths.');
    const results = [];
    for (const view of [0, 1]) {
        const run = (options) => JSON.parse(execFileSync(process.execPath, [__filename, '--trace', JSON.stringify(options)], { maxBuffer: 1024 * 1024 }));
        const reference = run({ js: referenceJs, wasm: referenceWasm, rom, checkpoint, view, grow: false });
        const candidate = run({ js, wasm, rom, checkpoint, view, grow: true });
        assert.equal(candidate.initialHeap, 128 * 1024 * 1024);
        assert.deepEqual(candidate.samples, reference.samples, `Seat ${view + 1} differed from the fixed-memory reference.`);
        results.push({ view, reference, candidate });
    }
    // Opposite local views can differ in presentation/savestate buffers, but
    // must agree on frame/RAM and each owner's persistent cartridge data.
    const simulation = (result) => result.candidate.samples.map(({ frame, ram, saves }) => ({ frame, ram, saves }));
    assert.deepEqual(simulation(results[0]), simulation(results[1]));
    fs.writeFileSync(output, JSON.stringify({ ok: true, results }, null, 2) + '\n');
    console.log(`Cooperative memory, recovery, output and opposite-view proof passed: ${output}`);
}
