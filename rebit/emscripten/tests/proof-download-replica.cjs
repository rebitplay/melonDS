// Local mixed cartridge/firmware determinism oracle. No Internet radio is used.
// RSFT contains the seat-local software-renderer presentation state. The two
// replicas may present different seats, so the byte oracle compares simulation
// snapshots with one common view and separately checks that changing the view
// leaves the simulation hash unchanged.
// Requires user-supplied ROM/BIOS files; never downloads or modifies those files.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { deflateSync } = require('node:zlib');
const { performance } = require('node:perf_hooks');
const { difference, sha } = require('./state-difference.cjs');

const env = process.env;
for (const key of ['MELONDS_DUAL_JS', 'MELONDS_DUAL_WASM', 'NDS_ROM', 'NDS_BIOS7', 'NDS_BIOS9', 'NDS_FIRMWARE', 'NDS_PROOF_OUTPUT'])
    assert.ok(env[key], `${key} is required`);
const frames = Number(env.NDS_PROOF_FRAMES || 6000);
assert.ok(Number.isInteger(frames) && frames > 0 && frames <= 36000);
const output = path.resolve(env.NDS_PROOF_OUTPUT);
fs.mkdirSync(output, { recursive: true });
const assets = ['NDS_ROM', 'NDS_BIOS7', 'NDS_BIOS9', 'NDS_FIRMWARE'].map(key => fs.readFileSync(env[key]));
const wasm = fs.readFileSync(env.MELONDS_DUAL_WASM);
const cores = [];
const frameTimes = [[], []];
const report = { ok: false, scope: 'Mixed Download Play cold boot; screenshots require review for game completion', frames,
    wasmSha256: sha(wasm), assets: assets.map(sha), comparisons: 0, sharedMemory: false };
function withBytes(core, bytes, action) {
    const ptr = core._malloc(bytes.length);
    assert.ok(ptr);
    try { core.HEAPU8.set(bytes, ptr); return action(ptr); }
    finally { core._free(ptr); }
}
const error = core => core.UTF8ToString(core._md_last_error());
function snapshot(core) {
    if (typeof core._md_rollback_save_slot !== 'function') {
        assert.equal(core._md_export_checkpoint(), 1, error(core));
        const ptr = core._md_checkpoint_data(), size = core._md_checkpoint_size();
        assert.ok(ptr && size);
        const bytes = Buffer.from(core.HEAPU8.subarray(ptr, ptr + size));
        core._md_clear_checkpoint();
        return [bytes];
    }
    assert.equal(core._md_rollback_save_slot(0, core._md_frame(0)), 1, error(core));
    return [0, 1, 2].map(part => {
        const ptr = core._md_rollback_part_data(0, part), size = core._md_rollback_part_size(0, part);
        assert.ok(ptr && size);
        return Buffer.from(core.HEAPU8.subarray(ptr, ptr + size));
    });
}
function crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type, bytes) {
    const data = Buffer.concat([Buffer.from(type), bytes]);
    const header = Buffer.alloc(4), tail = Buffer.alloc(4);
    header.writeUInt32BE(bytes.length); tail.writeUInt32BE(crc32(data));
    return Buffer.concat([header, data, tail]);
}
function screenshot(core, seat, frame) {
    const ptr = core._md_framebuffer(seat), width = 256, height = 384;
    const rows = Buffer.alloc((width * 4 + 1) * height);
    for (let y = 0; y < height; y++)
        rows.set(core.HEAPU8.subarray(ptr + y * width * 4, ptr + (y + 1) * width * 4), y * (width * 4 + 1) + 1);
    const header = Buffer.alloc(13);
    header.writeUInt32BE(width); header.writeUInt32BE(height, 4); header[8] = 8; header[9] = 6;
    fs.writeFileSync(path.join(output, `frame-${frame}-seat-${seat}.png`), Buffer.concat([
        Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', header), chunk('IDAT', deflateSync(rows)), chunk('IEND', Buffer.alloc(0)),
    ]));
}
// Super Mario 64 DS USA: enter firmware Download Play, host VS, select and
// confirm the title, then start the host transfer. These are ordinary inputs.
function input(frame, seat) {
    const touch = (start, end, x, y) => frame >= start && frame < end ? [0xfff, 1, x, y] : null;
    if (seat === 1) {
        // Firmware reaches its Download Play icon after the initial BIOS
        // animation; keep the tap late enough that it cannot be lost.
        const selected = touch(720, 1200, 176, 92);
        if (selected) return selected;
        // Select the advertised title, then press the firmware Select button.
        const title = touch(1800, 2050, 120, 55);
        if (title) return title;
        const select = touch(2100, 2400, 180, 170);
        if (select) return select;
        if ((frame >= 2500 && frame < 2700)) return [0xffe, 0, 0, 0];
    } else {
        // Start VS, reject the single-player practice prompt, then confirm
        // the match once the guest has completed its download boot.
        const selected = touch(720, 1400, 128, 96) || touch(1500, 1800, 30, 170)
            || ((frame >= 2000 && frame < 2200) ? [0xffd, 0, 0, 0] : null);
        if (selected) return selected;
    }
    return [0xfff, 0, 0, 0];
}
function compare(frame) {
    const states = cores.map(snapshot);
    const mismatch = difference(states[0], states[1]);
    report.comparisons++;
    if (mismatch) throw new Error(`Replica difference at frame ${frame}: ${JSON.stringify(mismatch)}`);
    report.stateHashes = states[0].map(sha);
}
async function main() {
    const loaded = require(path.resolve(env.MELONDS_DUAL_JS)), factory = loaded.default || loaded;
    for (let replica = 0; replica < 2; replica++) {
        const core = await factory({ wasmBinary: wasm, print: () => {}, printErr: text => { if (/OVERFLOW|error/i.test(text)) console.error(text); } });
        cores.push(core);
        assert.equal(core.UTF8ToString(core._md_build_id()), env.NDS_EXPECT_BUILD || 'melonds-dual-cooperative-2');
        assert.equal(core.HEAPU8.buffer instanceof SharedArrayBuffer, false);
        const pointers = assets.map(bytes => { const ptr = core._malloc(bytes.length); assert.ok(ptr); core.HEAPU8.set(bytes, ptr); return ptr; });
        try {
            assert.equal(core._md_load_with_profile(pointers[0], assets[0].length, 2, 0x12345678, 0x9abcdef0, 3,
                pointers[1], assets[1].length, pointers[2], assets[2].length, pointers[3], assets[3].length), 1, error(core));
        } finally { pointers.forEach(ptr => core._free(ptr)); }
        // Keep the strict serialized-state comparison on one common
        // presentation view. The product peers select opposite seats; that
        // changes only the local renderer's RSFT presentation bytes.
        core._md_set_visible_player(0);
        if (typeof core._md_rollback_configure === 'function') assert.ok(core._md_rollback_configure(2), error(core));
    }
    compare(0);
    const commonViewHashes = cores.map(core => core._md_state_hash());
    cores[1]._md_set_visible_player(1);
    assert.equal(cores[1]._md_state_hash(), commonViewHashes[1]);
    cores[1]._md_set_visible_player(0);
    assert.equal(cores[1]._md_state_hash(), commonViewHashes[1]);
    report.viewIndependentStateHash = true;
    for (let frame = 0; frame < frames; frame++) {
        for (let replica = 0; replica < 2; replica++) {
            const core = cores[replica];
            for (let seat = 0; seat < 2; seat++) core._md_set_input(seat, ...input(frame, seat));
            const start = performance.now();
            assert.equal(core._md_run_frame(), 1, error(core));
            frameTimes[replica].push(performance.now() - start);
            assert.equal(core._md_frame(0), frame + 1);
            assert.equal(core._md_frame(1), frame + 1);
            assert.equal(core._md_mp_outgoing_size(), 0, 'Replicated radio must stay local');
        }
        if ((frame + 1) % 120 === 0 || frame + 1 === frames) {
            compare(frame + 1);
            for (let seat = 0; seat < 2; seat++) screenshot(cores[0], seat, frame + 1);
            console.log(JSON.stringify({ frame: frame + 1, comparisons: report.comparisons,
                commands: cores[0]._md_mp_commands(0), replies: cores[0]._md_mp_replies(1) }));
        }
    }
    report.performance = frameTimes.map(times => {
        const ordered = times.slice(120).sort((a, b) => a - b);
        const p50 = ordered[Math.floor(ordered.length * 0.5)];
        return { coreFrameMsP50: p50, coreFrameMsP95: ordered[Math.floor(ordered.length * 0.95)], uncappedCoreFps: 1000 / p50 };
    });
    report.radio = cores.map(core => [0, 1].map(seat => ({ commands: core._md_mp_commands(seat), replies: core._md_mp_replies(seat) })));
    report.ok = true;
}
main().catch(err => { report.error = err.stack; process.exitCode = 1; }).finally(() => {
    cores.forEach(core => core._md_destroy());
    fs.writeFileSync(path.join(output, 'result.json'), JSON.stringify(report, null, 2));
    console.log(JSON.stringify(report));
});
