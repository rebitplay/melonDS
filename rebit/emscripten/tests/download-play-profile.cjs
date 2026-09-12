// Verifies the shared melonDS WASM core's Download Play boot profiles. This
// test only checks firmware boot and API separation; it does not claim an
// Internet Download Play session.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const env = process.env;
for (const name of ['MELONDS_DUAL_JS', 'MELONDS_DUAL_WASM', 'MKDS_ROM', 'NDS_BIOS7', 'NDS_BIOS9', 'NDS_FIRMWARE'])
    assert.ok(env[name], `${name} is required`);

const bytes = (name) => fs.readFileSync(env[name]);
const error = (core) => core.UTF8ToString(core._md_last_error());
function withBytes(core, value, callback) {
    const pointer = core._malloc(value.length);
    assert.ok(pointer);
    let advancedFrame = 0;
    try {
        core.HEAPU8.set(value, pointer);
        return callback(pointer);
    } finally {
        core._free(pointer);
    }
}

async function main() {
    const absolute = path.resolve(env.MELONDS_DUAL_JS);
    const loaded = require(absolute);
    const factory = loaded.default ?? loaded;
    const core = await factory({
        wasmBinary: fs.readFileSync(env.MELONDS_DUAL_WASM),
        mainScriptUrlOrBlob: absolute,
        print: () => {},
        printErr: () => {},
    });
    const rom = bytes('MKDS_ROM');
    assert.equal(
        withBytes(core, rom, (pointer) => core._md_load(pointer, rom.length, 2, 0x12345678, 0x9abcdef0)),
        1,
        error(core),
    );
    assert.equal(core._md_boot_profile(), 0);
    assert.equal(core._md_player_count(), 2);
    core._md_destroy();

    const bios7 = bytes('NDS_BIOS7');
    const bios9 = bytes('NDS_BIOS9');
    const firmware = bytes('NDS_FIRMWARE');
    const pointers = [bios7, bios9, firmware].map((value) => {
        const pointer = core._malloc(value.length);
        assert.ok(pointer);
        core.HEAPU8.set(value, pointer);
        return pointer;
    });
    try {
        assert.equal(
            core._md_load_with_profile(
                0,
                0,
                1,
                0x12345678,
                0x9abcdef0,
                2,
                pointers[0],
                bios7.length,
                pointers[1],
                bios9.length,
                pointers[2],
                firmware.length,
            ),
            1,
            error(core),
        );
        assert.equal(core._md_boot_profile(), 2);
        assert.equal(core._md_player_count(), 1);
        const startFrame = core._md_frame(0);
        const result = core._md_run_frame();
        assert.ok(result === 1 || result === 2, error(core));
        advancedFrame = core._md_frame(0);
        // A firmware guest may reach the first external-radio receive boundary
        // before a host packet exists.  That is a resumable network wait, not
        // a failed frame; once a frame completes its clock advances normally.
        assert.ok(advancedFrame === startFrame || advancedFrame === startFrame + 1);
        assert.equal(core._md_boot_profile(), 2);
    } finally {
        core._md_destroy();
        for (const pointer of pointers) core._free(pointer);
    }
    console.log(JSON.stringify({ ok: true, profile: 'download-client', frame: advancedFrame }));
}

main().catch((error) => {
    console.error(error.stack || error);
    process.exitCode = 1;
});
