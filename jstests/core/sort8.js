// Check sorting of arrays indexed by key SERVER-2884

t = db.jstests_sort8;
t.drop();

t.save({a: [1, 10]});
t.save({a: 5});
unindexedForward = t.find().sort({a: 1}).toArray();
unindexedReverse = t.find().sort({a: -1}).toArray();
t.ensureIndex({a: 1});
indexedForward = t.find().sort({a: 1}).hint({a: 1}).toArray();
indexedReverse = t.find().sort({a: -1}).hint({a: 1}).toArray();

assert.eq(unindexedForward, indexedForward);
assert.eq(unindexedReverse, indexedReverse);

// Sorting is based on array members, not the array itself.
assert.eq([1, 10], unindexedForward[0].a);
assert.eq([1, 10], unindexedReverse[0].a);

// Now try with a bounds constraint.
t.dropIndexes();
unindexedForward = t.find({a: {$gte: 5}}).sort({a: 1}).toArray();
unindexedReverse = t.find({a: {$lte: 5}}).sort({a: -1}).toArray();
t.ensureIndex({a: 1});
indexedForward = t.find({a: {$gte: 5}}).sort({a: 1}).hint({a: 1}).toArray();
indexedReverse = t.find({a: {$lte: 5}}).sort({a: -1}).hint({a: 1}).toArray();

assert.eq(unindexedForward, indexedForward);
assert.eq(unindexedReverse, indexedReverse);
