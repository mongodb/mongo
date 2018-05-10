// Cannot implicitly shard accessed collections because of collection existing when none
// expected.  Also, the primary node cannot change because we use the local database in this test.
// @tags: [assumes_no_implicit_collection_creation_after_drop, does_not_support_stepdowns]

t = db.jstests_index9;

t.drop();
assert.commandWorked(db.createCollection("jstests_index9"));
assert.eq(1, t.getIndexes().length, "There should be 1 index with default collection");
t.drop();
assert.commandWorked(db.createCollection("jstests_index9", {autoIndexId: true}));
assert.eq(1, t.getIndexes().length, "There should be 1 index if autoIndexId: true");

t.drop();
var t2 = db.getSiblingDB("local").jstests_index9;
assert.commandWorked(t2.getDB().createCollection("jstests_index9", {autoIndexId: false}));
assert.eq(0, t2.getIndexes().length, "There should be 0 index if autoIndexId: false");
assert.commandWorked(t2.createIndex({_id: 1}));
assert.eq(1, t2.getIndexes().length);
assert.commandWorked(t2.createIndex({_id: 1}));
assert.eq(1, t2.getIndexes().length);
t2.drop();

assert.commandWorked(t.createIndex({_id: 1}));
assert.eq(1, t.getIndexes().length);

t.drop();
t.save({a: 1});
assert.commandWorked(t.createIndex({_id: 1}));
assert.eq(1, t.getIndexes().length);
