// Cannot implicitly shard accessed collections because renameCollection command not supported
// on sharded collections.
// @tags: [
//   assumes_unsharded_collection,
//   requires_non_retryable_commands,
// ]

// Test that collections with text indexes can be renamed.  SERVER-14027.

let coll1 = db.fts_index2;
let coll2 = db.fts_index2.renamed;

coll1.drop();
coll2.drop();

assert.commandWorked(coll1.insert({a: {b: "some content"}}));
assert.commandWorked(coll1.createIndex({"$**": "text"}));
assert.eq(1, coll1.count({$text: {$search: "content"}}));

// Rename within same database.
assert.commandWorked(coll1.getDB().adminCommand({renameCollection: coll1.getFullName(), to: coll2.getFullName()}));
assert.eq(1, coll2.count({$text: {$search: "content"}}));
