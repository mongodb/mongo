// Test that:
// 1. Text indexes properly validate the index spec used to create them.
// 2. Text indexes properly enforce a schema on the language_override field.

load("jstests/libs/fts.js");

var coll = db.fts_index;
var indexName = "textIndex";
coll.drop();
coll.getDB().createCollection(coll.getName());

//
// 1. Text indexes properly validate the index spec used to create them.
//

// Spec passes text-specific index validation.
coll.ensureIndex({a: "text"}, {name: indexName, default_language: "spanish"});
assert(!db.getLastError());
assert.eq(1, coll.getDB().system.indexes.count({ns: coll.getFullName(), name: indexName}));
coll.dropIndexes();

// Spec fails text-specific index validation ("spanglish" unrecognized).
coll.ensureIndex({a: "text"}, {name: indexName, default_language: "spanglish"});
assert(db.getLastError());
assert.eq(0, coll.system.indexes.count({ns: coll.getFullName(), name: indexName}));
coll.dropIndexes();

// Spec passes general index validation.
coll.ensureIndex({"$**": "text"}, {name: indexName});
assert(!db.getLastError());
assert.eq(1, coll.getDB().system.indexes.count({ns: coll.getFullName(), name: indexName}));
coll.dropIndexes();

// Spec fails general index validation ("a.$**" invalid field name for key).
coll.ensureIndex({"a.$**": "text"}, {name: indexName});
assert(db.getLastError());
assert.eq(0, coll.getDB().system.indexes.count({ns: coll.getFullName(), name: indexName}));
coll.dropIndexes();

//
// 2. Text indexes properly enforce a schema on the language_override field.
//

// Can create a text index on a collection where no documents have invalid language_override.
coll.insert({a: ""});
coll.insert({a: "", language: "spanish"});
coll.ensureIndex({a: "text"});
assert(!db.getLastError());
coll.drop();

// Can't create a text index on a collection containing document with an invalid language_override.
coll.insert({a: "", language: "spanglish"});
coll.ensureIndex({a: "text"});
assert(db.getLastError());
coll.drop();

// Can insert documents with valid language_override into text-indexed collection.
coll.ensureIndex({a: "text"});
assert(!db.getLastError());
coll.insert({a: ""});
coll.insert({a: "", language: "spanish"});
assert(!db.getLastError());
coll.drop();

// Can't insert documents with invalid language_override into text-indexed collection.
coll.ensureIndex({a: "text"});
assert(!db.getLastError());
coll.insert({a: "", language: "spanglish"});
assert(db.getLastError());
coll.drop();
