// Test removal of a substantial proportion of inserted documents.  SERVER-3803
// A complete test will only be performed against a DEBUG build.

t = db.jstests_removea;

Random.setRandomSeed();

for (v = 0; v < 2; ++v) {  // Try each index version.
    t.drop();
    t.ensureIndex({a: 1}, {v: v});
    S = 100;
    B = 100;
    for (var x = 0; x < S; x++) {
        var batch = [];
        for (var y = 0; y < B; y++) {
            var i = y + (B * x);
            batch.push({a: i});
        }
        t.insert(batch);
    }
    assert.eq(t.count(), S * B);

    toDrop = [];
    for (i = 0; i < S * B; ++i) {
        toDrop.push(Random.randInt(10000));  // Dups in the query will be ignored.
    }
    // Remove many of the documents; $atomic prevents use of a ClientCursor, which would invoke a
    // different bucket deallocation procedure than the one to be tested (see SERVER-4575).
    var res = t.remove({a: {$in: toDrop}, $atomic: true});
    assert.writeOK(res);
}
