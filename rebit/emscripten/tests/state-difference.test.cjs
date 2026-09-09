const { test } = require('node:test');
const assert = require('node:assert/strict');
const { difference, sections } = require('./state-difference.cjs');

test('strict comparator detects every byte, including renderer/cache and runtime bytes', () => {
    const state = Buffer.alloc(64);
    state.write('RSFT', 16);
    state.writeUInt32LE(48, 20);
    const expected = [state, Buffer.alloc(32), Buffer.alloc(96)];
    assert.equal(difference(expected, expected.map(Buffer.from)), null);
    for (let part = 0; part < expected.length; ++part)
        for (let offset = 0; offset < expected[part].length; ++offset) {
            const actual = expected.map((bytes) => Buffer.from(bytes));
            actual[part][offset] ^= 1;
            assert.equal(difference(expected, actual).part, part);
            assert.equal(difference(expected, actual).offset, offset);
        }
    const changed = Buffer.from(state);
    changed[58] ^= 1; // No old RSFT trailing-cache-byte exemption.
    assert.equal(difference([state], [changed]).section, 'RSFT');
});

test('section coverage rejects missing/truncated metadata rather than silently comparing partial state', () => {
    const bytes = Buffer.alloc(48);
    bytes.write('MELN'); bytes.writeUInt32LE(48, 8);
    bytes.write('RTIM', 16); bytes.writeUInt32LE(16, 20);
    bytes.write('LMP.', 32); bytes.writeUInt32LE(16, 36);
    assert.deepEqual(sections(bytes).map(s => s.name), ['RTIM', 'LMP.']);
    assert.throws(() => sections(bytes.subarray(0, 47)), /container/);
    bytes.writeUInt32LE(64, 36);
    assert.throws(() => sections(bytes), /geometry/);
});

test('length and part-count mismatches fail, even with invalid section headers', () => {
    assert.ok(difference([Buffer.alloc(2)], [Buffer.alloc(3)]));
    assert.ok(difference([Buffer.alloc(2)], []));
    const a = Buffer.alloc(32), b = Buffer.alloc(32);
    b[31] = 1;
    assert.equal(difference([a], [b]).offset, 31);
});
