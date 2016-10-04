// Test that collections with text indexes can be renamed.  SERVER-14027.

var coll1 = db.fts_index2;
var coll2 = db.fts_index2.renamed;

coll1.drop();
coll2.drop();

assert.writeOK(coll1.insert({a: {b: "some content"}}));
assert.commandWorked(coll1.ensureIndex({"$**": "text"}));
assert.eq(1, coll1.count({$text: {$search: "content"}}));

// Rename within same database.
assert.commandWorked(
    coll1.getDB().adminCommand({renameCollection: coll1.getFullName(), to: coll2.getFullName()}));
assert.eq(1, coll2.count({$text: {$search: "content"}}));
