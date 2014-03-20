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

assert.eq(4, t.find().batchSize(2).itcount(), 'E');
assert.eq(4, t.find().sort({a: 1}).batchSize(2).itcount(), 'F');

// Run the tests with the index twice in order to double check plan caching.
t.ensureIndex({a: 1});
for (var i = 0; i < 2; i++) {
    runIndexedTests();
}

// The next tests make sure that we obey limit and batchSize properly when
// the sort could be either indexed or unindexed.
t.drop();
t.ensureIndex({a: 1});
t.ensureIndex({b: 1});

for (var i = 0; i < 100; i++) {
    t.save({_id: i, a: i, b: 1});
}

// Without a hint. Do it twice to make sure caching is ok.
for (var i = 0; i < 2; i++) {
    assert.eq(15, t.find({a: {$gte: 85}}).sort({b: 1}).batchSize(2).itcount(), 'K');
    assert.eq(6, t.find({a: {$gte: 85}}).sort({b: 1}).limit(6).itcount(), 'L');
}

// Hinting 'a'.
assert.eq(15, t.find({a: {$gte: 85}}).sort({b: 1}).hint({a: 1}).batchSize(2).itcount(), 'M');
assert.eq(6, t.find({a: {$gte: 85}}).sort({b: 1}).hint({a: 1}).limit(6).itcount(), 'N');

// Hinting 'b'.
assert.eq(15, t.find({a: {$gte: 85}}).sort({b: 1}).hint({b: 1}).batchSize(2).itcount(), 'O');
assert.eq(6, t.find({a: {$gte: 85}}).sort({b: 1}).hint({b: 1}).limit(6).itcount(), 'P');

// With explain.
assert.eq(15, t.find({a: {$gte: 85}}).sort({b: 1}).batchSize(2).explain().n, 'Q');
assert.eq(6, t.find({a: {$gte: 85}}).sort({b: 1}).limit(6).explain().n, 'R');

// Double check that we're not scanning more stuff than we have to.
// In order to get the sort using index 'a', we should need to scan
// about 50 keys and 50 documents.
var explain = t.find({a: {$gte: 50}}).sort({b: 1}).hint({a: 1}).limit(6).explain();
assert.lte(explain.nscanned, 60, 'S');
assert.lte(explain.nscannedObjects, 60, 'T');
assert.eq(explain.n, 6, 'U');
