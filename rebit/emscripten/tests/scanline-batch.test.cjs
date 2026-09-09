const assert = require('node:assert/strict');
const { test } = require('node:test');
const { readFileSync } = require('node:fs');
const { resolve } = require('node:path');

test('scanline batches publish exactly 192 tokens, never before the final pass', () => {
    const source = readFileSync(resolve(__dirname, '../../../src/GPU3D_Soft.cpp'), 'utf8');
    const body = source.split('void SoftRenderer3D::RenderPolygons(')[1].split('void SoftRenderer3D::FinishRendering')[0];
    assert.match(body, /constexpr int scanlineBatch = 8;/);
    assert.match(body, /constexpr int scanlineBatch = 1;/);
    assert.match(body, /ScanlineFinalPass\(y - 1\);[\s\S]*if \(threaded && y % scanlineBatch == 0\)/);
    assert.match(body, /ScanlineFinalPass\(191\);\s*if \(threaded\)\s*Platform::Semaphore_Post\(Sema_ScanlineCount, scanlineBatch\);/);
    for (const batch of [1, 8]) {
        let published = 0, notifications = 0;
        for (let y = 1; y < 192; ++y) {
            if (y % batch === 0) { published += batch; ++notifications; }
            assert.ok(published <= y, 'Unfinished row released');
            assert.ok(y - published < batch, 'Batch of finished rows was not released');
        }
        published += batch; ++notifications;
        assert.equal(published, 192);
        assert.equal(notifications, 192 / batch);
    }
});
