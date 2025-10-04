// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
//  assumes_no_implicit_index_creation,
//  requires_fastcount,
//  requires_getmore,
// ]

let t = db.index_sparse2;
t.drop();

t.insert({_id: 1, x: 1, y: 1});
t.insert({_id: 2, x: 2});
t.insert({_id: 3});

t.createIndex({x: 1, y: 1});
assert.eq(2, t.getIndexes().length, "A1");
assert.eq(3, t.find().sort({x: 1, y: 1}).count(), "A2 count()");
assert.eq(3, t.find().sort({x: 1, y: 1}).itcount(), "A2 itcount()");
t.dropIndex({x: 1, y: 1});
assert.eq(1, t.getIndexes().length, "A3");

t.createIndex({x: 1, y: 1}, {sparse: 1});
assert.eq(2, t.getIndexes().length, "B1");
assert.eq(3, t.find().sort({x: 1, y: 1}).count(), "B2 count()");
assert.eq(3, t.find().sort({x: 1, y: 1}).itcount(), "B2 itcount()");
t.dropIndex({x: 1, y: 1});
assert.eq(1, t.getIndexes().length, "B3");
