// Test subtleties of batchSize and limit.

var t = db.jstests_batch_size;
t.drop();

for (var i = 0; i < 4; i++) {
    t.save({_id: i, a: i});
}

function runIndexedTests() {
    // With limit, indexed.
    assert.eq(2, t.find().limit(2).itcount(), 'G');
    assert.eq(2, t.find().sort({a: 1}).limit(2).itcount(), 'H');

    // With batchSize, indexed.
    // SERVER-12438: If there is an index that provides the sort,
    // then a plan with an unindexed sort should never be used.
    // Consequently, batchSize will NOT be a hard limit in this case.
    // WARNING: the behavior described above may change in the future.
    assert.eq(4, t.find().batchSize(2).itcount(), 'I');
    assert.eq(4, t.find().sort({a: 1}).batchSize(2).itcount(), 'J');
}

// Without batch size or limit, unindexed.
assert.eq(4, t.find().itcount(), 'A');
assert.eq(4, t.find().sort({a: 1}).itcount(), 'B');

// With limit, unindexed.
assert.eq(2, t.find().limit(2).itcount(), 'C');
assert.eq(2, t.find().sort({a: 1}).limit(2).itcount(), 'D');

// With batchSize, unindexed.
// SERVER-12438: in general batch size does not mean a hard
// limit. With an unindexed sort, however, the server interprets
// batch size as a hard limit so that it can do a top k sort.
// WARNING: this behavior may change in the future.
assert.eq(4, t.find().batchSize(2).itcount(), 'E');
assert.eq(2, t.find().sort({a: 1}).batchSize(2).itcount(), 'F');

// Run the tests with the index twice in order to double check plan caching.
t.ensureIndex({a: 1});
for (var i = 0; i < 2; i++) {
    runIndexedTests();
}

