// Compile the exact production method, not a separately maintained fast path.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { test } = require('node:test');
const root = path.resolve(__dirname, '../../..');

test('ARM7 fast fetch preserves live mapping, alignment and fallback routing in WASM', () => {
    const source = fs.readFileSync(path.join(root, 'src/ARM.cpp'), 'utf8');
    const method = source.match(/u32 ARMv4::CodeRead32\(u32 addr\)\n\{[\s\S]*?\n\}/)?.[0];
    assert.ok(method, 'Missing production method');
    const output = fs.mkdtempSync(path.join(require('node:os').tmpdir(), 'dual-arm7-fetch-'));
    fs.writeFileSync(path.join(output, 'ARM7FastFetch.inc'), `namespace melonDS {\n${method}\n}\n`);
    const executable = path.join(output, 'test.cjs');
    const compiler = path.join(process.env.EMSCRIPTEN || '/home/daudau/Code/emsdk/upstream/emscripten', 'em++');
    execFileSync(compiler, ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
        '-sENVIRONMENT=node', '-sSINGLE_FILE=1', path.join(__dirname, 'arm7-fetch.test.cpp'),
        '-I', output, '-o', executable]);
    const result = execFileSync(process.execPath, [executable], { encoding: 'utf8' });
    assert.match(result, /PASS: 800136 /);
    console.log(result.trim(), `(${output})`);
});
