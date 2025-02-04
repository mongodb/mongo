// @tags: [requires_non_retryable_writes, requires_fastcount]

// Test removal of a substantial proportion of inserted documents.
const t = db[jsTestName()];

Random.setRandomSeed();

for (let v = 0; v < 2; ++v) {  // Try each index version.
    t.drop();
    const indexCreateRes = t.createIndex({a: 1}, {v: v});
    if (v === 0) {
        assert.commandFailedWithCode(indexCreateRes, ErrorCodes.CannotCreateIndex);
    } else {
        assert.commandWorked(indexCreateRes);
    }
    const S = 100;
    const B = 100;
    for (let x = 0; x < S; x++) {
        let batch = [];
        for (let y = 0; y < B; y++) {
            let i = y + (B * x);
            batch.push({a: i});
        }
        assert.commandWorked(t.insert(batch));
    }
    assert.eq(t.count(), S * B);

    let toDrop = [];
    for (let i = 0; i < S * B; ++i) {
        toDrop.push(Random.randInt(10000));  // Dups in the query will be ignored.
    }
    assert.commandWorked(t.remove({a: {$in: toDrop}}));
}