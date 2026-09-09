// Same-checkpoint, alternating-order CPU benchmark and strict cross-build oracle.
// Times md_run_frame only: not browser FPS, WebRTC, snapshots, or presentation.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { performance } = require('node:perf_hooks');
const { sha } = require('./state-difference.cjs');

const env = process.env;
for (const key of ['NDS_BASELINE_JS', 'NDS_CANDIDATE_JS', 'MKDS_ROM', 'MKDS_CHECKPOINT', 'NDS_CPU_OUTPUT'])
    assert.ok(env[key], `${key} is required`);
const rom = fs.readFileSync(env.MKDS_ROM), checkpoint = fs.readFileSync(env.MKDS_CHECKPOINT);
const warmup = 120, frames = 600, repetitions = 3;
const cores = [];
const startFrames = new Map();
const report = {
    ok: false, scope: 'Node, two linked consoles per runtime; md_run_frame timing, not browser FPS',
    environment: { node: process.version, platform: process.platform, arch: process.arch },
    warmup, frames, repetitions, romSha256: sha(rom), checkpointSha256: sha(checkpoint),
    reset: 'Import once per module, then restore the same full rollback slot before every repetition',
    builds: [], runs: [], comparisons: 0,
};
const err = core => core.UTF8ToString(core._md_last_error());
function withBytes(core, bytes, fn) {
    const ptr = core._malloc(bytes.length);
    assert.ok(ptr);
    try { core.HEAPU8.set(bytes, ptr); return fn(ptr); }
    finally { core._free(ptr); }
}
function state(core) {
    assert.equal(core._md_rollback_save_slot(0, core._md_frame(0)), 1, err(core));
    const hashes = Array.from({ length: 3 }, (_, part) => {
        const ptr = core._md_rollback_part_data(0, part), size = core._md_rollback_part_size(0, part);
        assert.ok(ptr && size);
        return sha(core.HEAPU8.subarray(ptr, ptr + size));
    });
    for (let player = 0; player < 2; ++player) {
        const ptr = core._md_framebuffer(player);
        hashes.push(sha(core.HEAPU8.subarray(ptr, ptr + 256 * 384 * 4)));
    }
    return hashes;
}
function run(core) {
    assert.equal(core._md_rollback_load_slot(2, startFrames.get(core)), 1, err(core));
    const samples = [], checkpoints = [];
    for (let i = 0; i < warmup + frames; ++i) {
        for (let player = 0; player < 2; ++player) {
            // Accelerate both seats, with deterministic alternating steering.
            const input = 1 | ((Math.floor(i / 60) + player) % 2 ? 16 : 32);
            assert.equal(core._md_set_input(player, 0xfff ^ input, 0, 0, 0), 1);
        }
        const before = performance.now();
        const result = core._md_run_frame();
        const elapsed = performance.now() - before;
        assert.equal(result, 1, err(core));
        if (i >= warmup) samples.push(elapsed);
        if ((i + 1) % 60 === 0) checkpoints.push(state(core));
    }
    const sorted = [...samples].sort((a, b) => a - b);
    const meanMs = samples.reduce((sum, n) => sum + n, 0) / samples.length;
    return { checkpoints, samples, medianMs: (sorted[299] + sorted[300]) / 2,
        p95Ms: sorted[Math.ceil(sorted.length * 0.95) - 1], meanMs,
        coreThroughputFps: 1000 / meanMs };
}
async function main() {
    for (const js of [env.NDS_BASELINE_JS, env.NDS_CANDIDATE_JS]) {
        const absolute = path.resolve(js), wasm = fs.readFileSync(absolute.replace(/\.js$/, '.wasm'));
        const loaded = require(absolute);
        const core = await (loaded.default ?? loaded)({ wasmBinary: wasm, mainScriptUrlOrBlob: absolute, print: () => {} });
        cores.push(core);
        assert.equal(core.UTF8ToString(core._md_build_id()), 'melonds-dual-deterministic-1');
        assert.equal(core.UTF8ToString(core._md_runtime_abi()), 'rebit-melonds-deterministic-v1');
        const seed = 11150031900141442680n;
        assert.equal(withBytes(core, rom, ptr => core._md_load(ptr, rom.length, 2,
            Number(seed & 0xffffffffn), Number(seed >> 32n))), 1, err(core));
        assert.equal(withBytes(core, checkpoint, ptr => core._md_import_checkpoint(ptr, checkpoint.length)), 1, err(core));
        assert.ok(core._md_rollback_configure(3), err(core));
        startFrames.set(core, core._md_frame(0));
        assert.equal(core._md_rollback_save_slot(2, startFrames.get(core)), 1, err(core));
        report.builds.push({ js: absolute, jsSha256: sha(fs.readFileSync(absolute)), wasmSha256: sha(wasm), wasmBytes: wasm.length });
    }
    assert.notEqual(report.builds[0].wasmSha256, report.builds[1].wasmSha256, 'A/B must use different binaries');
    let reference;
    for (let repetition = 0; repetition < repetitions; ++repetition) {
        for (const index of repetition % 2 ? [1, 0] : [0, 1]) {
            const result = run(cores[index]);
            if (reference) {
                assert.deepEqual(result.checkpoints, reference, 'Full state or video differs across builds/runs');
                report.comparisons += result.checkpoints.length;
            } else reference = result.checkpoints;
            const row = { repetition, build: index ? (env.NDS_CANDIDATE_LABEL || 'compact') : 'baseline', ...result };
            report.runs.push(row);
            console.log(JSON.stringify({ repetition, build: row.build, medianMs: row.medianMs, meanMs: row.meanMs }));
        }
    }
    report.ok = true;
}
main().catch(error => { report.error = error.stack; process.exitCode = 1; }).finally(() => {
    for (const core of cores) core._md_destroy();
    fs.writeFileSync(env.NDS_CPU_OUTPUT, `${JSON.stringify(report, null, 2)}\n`);
    console.log(JSON.stringify({ ok: report.ok, comparisons: report.comparisons, error: report.error }));
});
