// Test dropping an index that is being used by an agg pipeline.
var coll = db.server14969;
var docsPerBatch = 3;
coll.drop();

// Initialize collection with eight 1M documents, and index on field "a".
var longString = new Array(1024 * 1024).join('x');
for (var i = 0; i < 100; ++i) {
    assert.writeOK(coll.insert({a: 1, bigField: longString}));
}
assert.commandWorked(coll.ensureIndex({a: 1}));

// Create pipeline that uses index "a", with a small initial batch size.
var cursor = coll.aggregate([{$match: {a: 1}}], {cursor: {batchSize: docsPerBatch}});
for (var i = 0; i < docsPerBatch; ++i) {
    assert(cursor.hasNext());
    assert.eq(1, cursor.next().a);
}

// Drop index "a".
assert.commandWorked(coll.dropIndex({a: 1}));

// Issue a getmore against agg cursor.  Note that it is not defined whether the server continues to
// generate further results for the cursor.
try {
    cursor.hasNext();
    cursor.next();
} catch (e) {
}

// Verify that the server hasn't crashed.
assert.commandWorked(db.adminCommand({ping: 1}));
