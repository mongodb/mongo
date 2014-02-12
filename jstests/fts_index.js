// Test that:
// 1. Text indexes properly validate the index spec used to create them.
// 2. Text indexes properly enforce a schema on the language_override field.
// 3. Collections may have at most one text index.
// 4. Text indexes properly handle large documents.

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

//
// 3. Collections may have at most one text index.
//

coll.ensureIndex({a: "text"});
assert(!db.getLastError());

coll.ensureIndex({a: "text"});
assert(db.getLastError());
coll.ensureIndex({b: "text"});
assert(db.getLastError());
coll.ensureIndex({b: "text", c: 1});
assert(db.getLastError());
coll.ensureIndex({b: 1, c: "text"});
assert(db.getLastError());

coll.dropIndexes();

coll.ensureIndex({a: 1, b: "text", c: 1});
assert(!db.getLastError());

coll.ensureIndex({a: 1, b: "text", c: 1});
assert(db.getLastError());
coll.ensureIndex({b: "text"});
assert(db.getLastError());
coll.ensureIndex({b: "text", c: 1});
assert(db.getLastError());
coll.ensureIndex({b: 1, c: "text"});
assert(db.getLastError());

coll.dropIndexes();

//
// 4. Text indexes properly handle large keys.
//

coll.ensureIndex({a: "text"});
assert(!db.getLastError());

var longstring = "";
var longstring2 = "";
for(var i = 0; i < 1024 * 1024; ++i) {
    longstring = longstring + "a";
    longstring2 = longstring2 + "b";
}
coll.insert({a: longstring});
coll.insert({a: longstring2});
assert.eq(1, coll.find({$text: {$search: longstring}}).itcount(), "long string not found in index");

coll.drop();
