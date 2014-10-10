// Test creation of an index with key pattern {_id: -1}.  It is expected that a request for creation
// of a {_id: -1} index is treated as if it were a request for creation of a {_id: 1} index.
// SERVER-14833.

var coll = db.index_id_desc;
var indexes;

// Test ensureIndex({_id: -1}) on a nonexistent collection.
coll.drop();
assert.commandWorked(coll.ensureIndex({_id: -1}));
indexes = coll.getIndexes();
assert.eq(1, indexes.length);
assert.eq("_id_", indexes[0].name);
assert.eq({_id: 1}, indexes[0].key);

// Test ensureIndex({_id: -1}) on a normal empty collection.
coll.drop();
assert.commandWorked(coll.runCommand("create"));
assert.eq(1, coll.getIndexes().length);
assert.commandWorked(coll.ensureIndex({_id: -1}));
indexes = coll.getIndexes();
assert.eq(1, indexes.length);
assert.eq("_id_", indexes[0].name);
assert.eq({_id: 1}, indexes[0].key);

// Test ensureIndex({_id: -1}) on an empty collection with no _id index.
coll.drop();
assert.commandWorked(coll.runCommand("create", {autoIndexId: false}));
assert.eq(0, coll.getIndexes().length);
assert.commandWorked(coll.ensureIndex({_id: -1}));
indexes = coll.getIndexes();
assert.eq(1, indexes.length);
assert.eq("_id_", indexes[0].name);
assert.eq({_id: 1}, indexes[0].key);
