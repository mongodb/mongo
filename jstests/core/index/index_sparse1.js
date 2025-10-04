// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
//  assumes_no_implicit_index_creation,
//  requires_non_retryable_writes,
//  requires_fastcount,
//  requires_getmore,
// ]

let t = db.index_sparse1;
t.drop();

t.insert({_id: 1, x: 1});
t.insert({_id: 2, x: 2});
t.insert({_id: 3, x: 2});
t.insert({_id: 4});
t.insert({_id: 5});

assert.eq(5, t.count(), "A1");
assert.eq(5, t.find().sort({x: 1}).itcount(), "A2");

t.createIndex({x: 1});
assert.eq(2, t.getIndexes().length, "B1");
assert.eq(5, t.find().sort({x: 1}).itcount(), "B2");
t.dropIndex({x: 1});
assert.eq(1, t.getIndexes().length, "B3");

t.createIndex({x: 1}, {sparse: 1});
assert.eq(2, t.getIndexes().length, "C1");
assert.eq(5, t.find().sort({x: 1}).itcount(), "C2");
t.dropIndex({x: 1});
assert.eq(1, t.getIndexes().length, "C3");

// -- sparse & unique

t.remove({_id: 2});

// test that we can't create a unique index without sparse
assert.commandFailed(t.createIndex({x: 1}, {unique: 1}));
assert.eq(1, t.getIndexes().length, "D2");

t.createIndex({x: 1}, {unique: 1, sparse: 1});
assert.eq(2, t.getIndexes().length, "E1");
t.dropIndex({x: 1});
assert.eq(1, t.getIndexes().length, "E3");

t.insert({_id: 2, x: 2});
t.createIndex({x: 1}, {unique: 1, sparse: 1});
assert.eq(1, t.getIndexes().length, "F1");
