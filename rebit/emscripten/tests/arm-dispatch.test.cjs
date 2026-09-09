// Exhaustively check generated dispatch against this core's canonical ISA table.
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { resolve } = require('node:path');
const { execFileSync } = require('node:child_process');
const { test } = require('node:test');

const root = resolve(__dirname, '../../..');
const table = readFileSync(resolve(root, 'src/ARM_InstrTable.h'), 'utf8')
    .replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, '');
const dispatch = readFileSync(resolve(root, 'src/ARM_InstrDispatch.h'), 'utf8');

for (const [name, count] of [['ARM', 4096], ['THUMB', 1024]]) {
    test(`${name}: all ${count} opcodes select the canonical direct handler`, () => {
        const canonical = table.match(new RegExp(
            `${name}InstrTable\\[${count}\\]\\)\\s*=\\s*\\{([\\s\\S]*?)\\};`,
        ))[1].split(',').map(s => s.trim()).filter(Boolean);
        assert.equal(canonical.length, count);
        const body = dispatch.split(`void Dispatch${name}(`)[1].split('\n}\n')[0];
        const ids = body.match(/handlers\[\]\s*=\s*\{([\s\S]*?)\};/)[1]
            .split(',').map(s => s.trim()).filter(Boolean).map(Number);
        const cases = [...body.matchAll(/case (\d+): return (\w+)\(cpu\);/g)];
        const handlers = new Map(cases.map(([, id, handler]) => [Number(id), handler]));
        assert.equal(handlers.size, cases.length, 'Duplicate handler IDs');
        assert.equal(ids.length, count);
        assert.match(body, /switch \(handlers\[code\]\)/);
        for (let code = 0; code < count; ++code) {
            assert.ok(Number.isInteger(ids[code]) && ids[code] >= 0 && ids[code] <= 65535);
            assert.equal(handlers.get(ids[code]), canonical[code], `${name} opcode ${code}`);
        }
    });
}

test('generated header is reproducible', () => {
    execFileSync('python3', [resolve(root, 'rebit/emscripten/generate_arm_dispatch.py'), '--check']);
});
