// Local authorized ROMs/traces only. Two separately initialized WASM modules,
// each owning the entire N-console system. No network, imported peer state,
// shared WASM memory, RAM-only oracle or byte exemptions.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { difference, sha, sections } = require('./state-difference.cjs');

const env = process.env;
for (const name of ['MELONDS_DUAL_JS', 'MELONDS_DUAL_WASM', 'MKDS_ROM', 'NDS_DETERMINISM_OUTPUT'])
    assert.ok(env[name], `${name} is required`);
const integer = (name, fallback, min, max) => {
    const value = Number(env[name] ?? fallback);
    assert.ok(Number.isInteger(value) && value >= min && value <= max, `Invalid ${name}`);
    return value;
};
const players = integer('NDS_DETERMINISM_PLAYERS', 2, 2, 4);
const frames = integer('NDS_DETERMINISM_FRAMES', 600, 1, 36000);
const window = integer('NDS_DETERMINISM_WINDOW', 4, 1, 16);
const expectedBuild = env.NDS_DETERMINISM_EXPECT_BUILD || 'melonds-dual-deterministic-1';
const timeline = env.NDS_DETERMINISM_TIMELINE
    ? JSON.parse(fs.readFileSync(env.NDS_DETERMINISM_TIMELINE)) : null;
assert.ok(!timeline || players === 2, 'Recorded two-console inputs cannot validate 3/4-player matches');
assert.ok(!timeline || !env.MKDS_CHECKPOINT, 'Choose cold-boot timeline or local checkpoint');
assert.ok(!timeline || timeline.inputs.length >= frames, 'Input timeline too short');
assert.ok(!timeline || timeline.confirmed + 1 >= frames, 'Do not treat predicted inputs as confirmed');
const seed = BigInt(timeline?.seed ?? '11150031900141442680');
const rom = fs.readFileSync(env.MKDS_ROM), wasm = fs.readFileSync(env.MELONDS_DUAL_WASM);
const cores = [];
const report = {
    ok: false, build: expectedBuild, players, frames, window,
    scope: timeline ? 'Cold-boot recorded two-console inputs, independent forward-only vs rollback replica'
        : env.MKDS_CHECKPOINT ? 'Local checkpoint, independent forward-only vs rollback replica'
          : 'Cold-boot synthetic inputs; NOT evidence of a connected multiplayer match',
    environment: { node: process.version, platform: process.platform, arch: process.arch },
    romSha256: sha(rom), wasmSha256: sha(wasm), jsSha256: sha(fs.readFileSync(env.MELONDS_DUAL_JS)),
    inputSha256: timeline ? sha(Buffer.from(JSON.stringify(timeline.inputs.slice(0, frames)))) : null,
    checks: 0, restores: 0, replayedFrames: 0, wrongFrames: 0, byteExemptions: 0,
};
const error = (core) => core.UTF8ToString(core._md_last_error());
function withBytes(core, bytes, call) {
    const ptr = core._malloc(bytes.length);
    assert.ok(ptr);
    try { core.HEAPU8.set(bytes, ptr); return call(ptr); }
    finally { core._free(ptr); }
}
function save(core, index) {
    const frame = core._md_frame(0);
    assert.equal(core._md_rollback_save_slot(index, frame), 1, error(core));
    return Array.from({ length: players + 1 }, (_, part) => {
        const size = core._md_rollback_part_size(index, part);
        const ptr = core._md_rollback_part_data(index, part);
        assert.ok(size && ptr);
        const bytes = Buffer.from(core.HEAPU8.subarray(ptr, ptr + size));
        if (part === players && expectedBuild === 'melonds-dual-deterministic-1') {
            const layout = sections(bytes);
            assert.deepEqual(layout.map(section => section.name), ['RTIM', 'LMP.']);
            assert.equal(layout[0].length, 24 + players * 58, 'Missing runtime/input/scheduler metadata');
            report.runtimeMetadataCovered = true;
        }
        return bytes;
    });
}
function compare(expected, actual, stage, frame) {
    const firstDifference = difference(expected, actual);
    ++report.checks;
    if (firstDifference) {
        report.failure = { stage, frame, ...firstDifference };
        throw new Error(`Determinism mismatch: ${JSON.stringify(report.failure)}`);
    }
}
function inputsAt(frame) {
    return timeline ? timeline.inputs[frame] : Array.from({ length: players }, (_, player) =>
        1 | (((Math.floor(frame / 30) + player) % 2) ? 16 : 32));
}
function advance(core, count, wrong, fragmentedAudio) {
    const outputs = [];
    for (let step = 0; step < count; ++step) {
        const frame = core._md_frame(0), inputs = inputsAt(frame);
        for (let player = 0; player < players; ++player) {
            // Force an actually different remote input; not a prediction that
            // accidentally equals the correct input throughout the test.
            const input = inputs[player] ^ (wrong && player === players - 1 ? 3 : 0);
            assert.equal(core._md_set_input(player, 0xfff ^ (input & 0xfff),
                (input >>> 12) & 1, (input >>> 13) & 255, input >>> 21), 1);
        }
        assert.equal(core._md_run_frame(), 1, error(core));
        for (let player = 0; player < players; ++player) assert.equal(core._md_frame(player), frame + 1);
        if (wrong) continue;
        const audio = [], video = [], saves = [];
        for (let player = 0; player < players; ++player) {
            const chunks = [];
            if (core._md_audio_read_player) {
                for (let count; (count = core._md_audio_read_player(player, fragmentedAudio ? 37 : 4096)) > 0;) {
                    const ptr = core._md_audio_buffer();
                    chunks.push(Buffer.from(core.HEAPU8.subarray(ptr, ptr + count * 4)));
                }
            }
            audio.push(sha(Buffer.concat(chunks)));
            const ptr = core._md_framebuffer(player);
            video.push(sha(core.HEAPU8.subarray(ptr, ptr + 256 * 384 * 4)));
            const cartridge = core._md_save_data(player);
            saves.push(sha(core.HEAPU8.subarray(cartridge, cartridge + core._md_save_size(player))));
        }
        outputs.push({ audio, video, saves });
    }
    return outputs;
}
async function main() {
    const loaded = require(path.resolve(env.MELONDS_DUAL_JS)), factory = loaded.default ?? loaded;
    for (let replica = 0; replica < 2; ++replica) {
        const core = await factory({ wasmBinary: wasm, mainScriptUrlOrBlob: path.resolve(env.MELONDS_DUAL_JS), print: () => {} });
        cores.push(core);
        assert.equal(core.UTF8ToString(core._md_build_id()), expectedBuild);
        if (expectedBuild === 'melonds-dual-deterministic-1') {
            assert.equal(core.UTF8ToString(core._md_runtime_abi()), 'rebit-melonds-deterministic-v1');
            assert.equal(typeof core._md_audio_read_player, 'function', 'Per-console audio oracle is required');
        }
        assert.equal(withBytes(core, rom, ptr => core._md_load(ptr, rom.length, players,
            Number(seed & 0xffffffffn), Number(seed >> 32n))), 1, error(core));
        if (env.MKDS_CHECKPOINT) {
            const checkpoint = fs.readFileSync(env.MKDS_CHECKPOINT);
            report.checkpointSha256 = sha(checkpoint);
            assert.equal(withBytes(core, checkpoint, ptr => core._md_import_checkpoint(ptr, checkpoint.length)), 1, error(core));
        }
        core._md_set_visible_player(replica ? players - 1 : 0);
        assert.equal(core._md_visible_player(), replica ? players - 1 : 0);
        core._md_set_scheduler_jitter(replica ? 7 : 1);
        const bytes = core._md_rollback_configure(3);
        assert.ok(bytes, error(core));
        report.snapshotBytes = bytes;
    }
    const [reference, branch] = cores;
    assert.notEqual(reference.HEAPU8.buffer, branch.HEAPU8.buffer, 'Replicas must not share memory');
    report.startFrame = reference._md_frame(0);
    compare(save(reference, 1), save(branch, 1), 'independent-initialization', report.startFrame);
    for (let elapsed = 0; elapsed < frames; elapsed += window) {
        const count = Math.min(window, frames - elapsed), at = branch._md_frame(0);
        const before = save(branch, 0);
        assert.equal(branch._md_rollback_load_slot(0, at + 1), 0, 'Wrong frame tag accepted');
        assert.equal(branch._md_rollback_save_slot(99, at), 0, 'Out-of-range slot accepted');
        branch._md_set_scheduler_jitter((at ^ 0x9e3779b9) >>> 0 || 1);
        advance(branch, count, true, false);
        report.wrongFrames += count;
        // Mutation stays inside this disposable WASM instance, never disk saves.
        for (let player = 0; player < players; ++player)
            if (branch._md_save_size(player)) branch.HEAPU8[branch._md_save_data(player)] ^= 0x5a;
        assert.equal(branch._md_rollback_load_slot(0, at), 1, error(branch));
        ++report.restores;
        compare(before, save(branch, 2), 'immediate-restore', at);
        branch._md_set_scheduler_jitter((at ^ 0x85ebca6b) >>> 0 || 1);
        const actualOutput = advance(branch, count, false, true);
        const expectedOutput = advance(reference, count, false, false);
        report.replayedFrames += count;
        const end = branch._md_frame(0);
        compare(save(reference, 1), save(branch, 2), 'independent-replay', end);
        assert.deepEqual(actualOutput, expectedOutput, `Audio/video/save output diverged at ${end}`);
        // Local view changes must leave saved simulation state valid and equal.
        branch._md_set_visible_player((Math.floor(elapsed / window) + 1) % players);
        assert.equal(branch._md_rollback_load_slot(2, end), 1, 'View change invalidated simulation checkpoint');
        compare(save(reference, 1), save(branch, 2), 'view-change', end);
        if (elapsed % 100 === 0) console.log(JSON.stringify({ frame: end, checks: report.checks }));
    }
    report.finalFrame = reference._md_frame(0);
    report.finalStateSha256 = save(reference, 1).map(sha);
    report.radio = cores.map(core => Array.from({ length: players }, (_, player) => ({
        sent: core._md_mp_packets_sent(player), received: core._md_mp_packets_received(player),
        commands: core._md_mp_commands(player), replies: core._md_mp_replies(player),
    })));
    // Negative controls must fail, including input state formerly invisible to
    // the console/radio byte oracle. Restore the disposable reference afterward.
    const pristine = save(reference, 0), finalFrame = reference._md_frame(0);
    assert.equal(reference._md_inject_desync_for_test(0, 0, 1), 1);
    assert.ok(difference(pristine, save(reference, 2)), 'Oracle missed guest RAM corruption');
    assert.equal(reference._md_rollback_load_slot(0, finalFrame), 1);
    assert.equal(reference._md_set_input(0, 0, 1, 250, 180), 1);
    const inputDifference = difference(pristine, save(reference, 2));
    assert.equal(inputDifference?.section, 'RTIM', 'Oracle missed pending runtime input state');
    assert.equal(reference._md_rollback_load_slot(0, finalFrame), 1);
    compare(pristine, save(reference, 2), 'negative-control-recovery', finalFrame);
    report.negativeControls = ['guest-ram', 'pending-input-metadata'];
    report.ok = true;
}
main().catch(e => { report.error = e.stack; process.exitCode = 1; }).finally(() => {
    for (const core of cores) core._md_destroy();
    fs.writeFileSync(env.NDS_DETERMINISM_OUTPUT, `${JSON.stringify(report, null, 2)}\n`);
    console.log(JSON.stringify(report, null, 2));
});
