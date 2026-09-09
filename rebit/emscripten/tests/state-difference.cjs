const { createHash } = require('node:crypto');

const sha = (bytes) => createHash('sha256').update(bytes).digest('hex');
function sections(bytes) {
    const result = [];
    if (bytes.length < 16 || bytes.toString('ascii', 0, 4) !== 'MELN' || bytes.readUInt32LE(8) !== bytes.length)
        throw new Error('Invalid snapshot container');
    for (let at = 16; at < bytes.length;) {
        if (at + 16 > bytes.length) throw new Error('Truncated section header');
        const length = bytes.readUInt32LE(at + 4);
        if (length < 16 || at + length > bytes.length) throw new Error('Invalid section geometry');
        result.push({ name: bytes.toString('ascii', at, at + 4), offset: at, length });
        at += length;
    }
    return result;
}
function sectionAt(bytes, offset) {
    // Savestate sections follow the 16-byte container header. Diagnostic only:
    // malformed geometry never causes bytes to be ignored by the comparator.
    for (let at = 16; at + 16 <= bytes.length;) {
        const length = bytes.readUInt32LE(at + 4);
        if (length < 16 || at + length > bytes.length) break;
        if (offset >= at && offset < at + length)
            return { section: bytes.toString('ascii', at, at + 4), relative: offset - at };
        at += length;
    }
    return {};
}

function difference(expected, actual) {
    if (expected.length !== actual.length) return { expectedParts: expected.length, actualParts: actual.length };
    for (let part = 0; part < expected.length; ++part) {
        const a = expected[part], b = actual[part];
        if (a.equals(b)) continue;
        let offset = 0;
        while (offset < Math.min(a.length, b.length) && a[offset] === b[offset]) ++offset;
        return {
            part, offset, ...sectionAt(a, offset),
            expected: a[offset], actual: b[offset],
            expectedLength: a.length, actualLength: b.length,
            expectedSha256: sha(a), actualSha256: sha(b),
        };
    }
    return null;
}

module.exports = { difference, sha, sections };
