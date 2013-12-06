// Test that a request to create a text index results in the expected index.

load("jstests/libs/fts.js");

var coll = db.fts_index;
var indexName = "textIndex";
coll.drop();
coll.getDB().createCollection(coll.getName());

// Passes text-specific index validation.
coll.ensureIndex({a: "text"}, {name: indexName, default_language: "spanish"});
assert(!db.getLastError());
assert.eq(1, coll.getDB().system.indexes.count({ns: coll.getFullName(), name: indexName}));
coll.dropIndexes();

// Fails text-specific index validation ("spanglish" unrecognized).
coll.ensureIndex({a: "text"}, {name: indexName, default_language: "spanglish"});
assert(db.getLastError());
assert.eq(0, coll.system.indexes.count({ns: coll.getFullName(), name: indexName}));
coll.dropIndexes();

// Passes general index validation.
coll.ensureIndex({"$**": "text"}, {name: indexName});
assert(!db.getLastError());
assert.eq(1, coll.getDB().system.indexes.count({ns: coll.getFullName(), name: indexName}));
coll.dropIndexes();

// Fails general index validation ("a.$**" invalid field name for key).
coll.ensureIndex({"a.$**": "text"}, {name: indexName});
assert(db.getLastError());
assert.eq(0, coll.getDB().system.indexes.count({ns: coll.getFullName(), name: indexName}));
coll.dropIndexes();
