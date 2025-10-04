// Test that updates to fields in a text-indexed document are correctly reflected in the text index.
const collNamePrefix = "fts_index3_";
let collCount = 0;

// 1) Create a text index on a single field, insert a document, update the value of the field, and
// verify that $text with the new value returns the document.
let coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: "text"}));
assert.commandWorked(coll.insert({a: "hello"}));
assert.eq(1, coll.find({$text: {$search: "hello"}}).itcount());
assert.commandWorked(coll.update({}, {$set: {a: "world"}}));
assert.eq(0, coll.find({$text: {$search: "hello"}}).itcount());
assert.eq(1, coll.find({$text: {$search: "world"}}).itcount());

// 2) Same as #1, but with a wildcard text index.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({"$**": "text"}));
assert.commandWorked(coll.insert({a: "hello"}));
assert.eq(1, coll.find({$text: {$search: "hello"}}).itcount());
assert.commandWorked(coll.update({}, {$set: {a: "world"}}));
assert.eq(0, coll.find({$text: {$search: "hello"}}).itcount());
assert.eq(1, coll.find({$text: {$search: "world"}}).itcount());

// 3) Create a compound text index with an index prefix, insert a document, update the value of the
// index prefix field, and verify that $text with the new value returns the document.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: 1, b: "text"}));
assert.commandWorked(coll.insert({a: 1, b: "hello"}));
assert.eq(1, coll.find({a: 1, $text: {$search: "hello"}}).itcount());
assert.commandWorked(coll.update({}, {$set: {a: 2}}));
assert.eq(0, coll.find({a: 1, $text: {$search: "hello"}}).itcount());
assert.eq(1, coll.find({a: 2, $text: {$search: "hello"}}).itcount());

// 4) Same as #3, but with a wildcard text index.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: 1, "$**": "text"}));
assert.commandWorked(coll.insert({a: 1, b: "hello"}));
assert.eq(1, coll.find({a: 1, $text: {$search: "hello"}}).itcount());
assert.commandWorked(coll.update({}, {$set: {a: 2}}));
assert.eq(0, coll.find({a: 1, $text: {$search: "hello"}}).itcount());
assert.eq(1, coll.find({a: 2, $text: {$search: "hello"}}).itcount());

// 5) Create a compound text index with an index suffix, insert a document, update the value of the
// index suffix field, and verify that $text with the new value returns the document.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: "text", b: 1}));
assert.commandWorked(coll.insert({a: "hello", b: 1}));
assert.eq(1, coll.find({b: 1, $text: {$search: "hello"}}).itcount());
assert.commandWorked(coll.update({}, {$set: {b: 2}}));
assert.eq(0, coll.find({b: 1, $text: {$search: "hello"}}).itcount());
assert.eq(1, coll.find({b: 2, $text: {$search: "hello"}}).itcount());

// 6) Same as #5, but with a wildcard text index.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({"$**": "text", b: 1}));
assert.commandWorked(coll.insert({a: "hello", b: 1}));
assert.eq(1, coll.find({b: 1, $text: {$search: "hello"}}).itcount());
assert.commandWorked(coll.update({}, {$set: {b: 2}}));
assert.eq(0, coll.find({b: 1, $text: {$search: "hello"}}).itcount());
assert.eq(1, coll.find({b: 2, $text: {$search: "hello"}}).itcount());

// 7) Create a text index on a single field, insert a document, update the language of the document
// (so as to change the stemming), and verify that $text with the new language returns the document.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: "text"}));
assert.commandWorked(coll.insert({a: "testing", language: "es"}));
assert.eq(1, coll.find({$text: {$search: "testing", $language: "es"}}).itcount());
assert.eq(0, coll.find({$text: {$search: "testing", $language: "en"}}).itcount());
assert.commandWorked(coll.update({}, {$set: {language: "en"}}));
assert.eq(0, coll.find({$text: {$search: "testing", $language: "es"}}).itcount());
assert.eq(1, coll.find({$text: {$search: "testing", $language: "en"}}).itcount());

// 8) Same as #7, but with a wildcard text index.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({"$**": "text"}));
assert.commandWorked(coll.insert({a: "testing", language: "es"}));
assert.eq(1, coll.find({$text: {$search: "testing", $language: "es"}}).itcount());
assert.eq(0, coll.find({$text: {$search: "testing", $language: "en"}}).itcount());
assert.commandWorked(coll.update({}, {$set: {language: "en"}}));
assert.eq(0, coll.find({$text: {$search: "testing", $language: "es"}}).itcount());
assert.eq(1, coll.find({$text: {$search: "testing", $language: "en"}}).itcount());

// 9) Create a text index on a single nested field, insert a document, update the language of the
// subdocument (so as to change the stemming), and verify that $text with the new language returns
// the document.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({"a.b": "text"}));
assert.commandWorked(coll.insert({a: {b: "testing", language: "es"}}));
assert.eq(1, coll.find({$text: {$search: "testing", $language: "es"}}).itcount());
assert.eq(0, coll.find({$text: {$search: "testing", $language: "en"}}).itcount());
assert.commandWorked(coll.update({}, {$set: {"a.language": "en"}}));
assert.eq(0, coll.find({$text: {$search: "testing", $language: "es"}}).itcount());
assert.eq(1, coll.find({$text: {$search: "testing", $language: "en"}}).itcount());

// SERVER-78238: index with a dotted path should not index fields with a dot inside that make it
// look like a dotted path.
assert.commandWorked(coll.insert({"a.b": "ignored"}));
assert.eq(0, coll.find({$text: {$search: "ignored"}}).itcount());

// 10) Same as #9, but with a wildcard text index.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({"$**": "text"}));
assert.commandWorked(coll.insert({a: {b: "testing", language: "es"}}));
assert.eq(1, coll.find({$text: {$search: "testing", $language: "es"}}).itcount());
assert.eq(0, coll.find({$text: {$search: "testing", $language: "en"}}).itcount());
assert.commandWorked(coll.update({}, {$set: {"a.language": "en"}}));
assert.eq(0, coll.find({$text: {$search: "testing", $language: "es"}}).itcount());
assert.eq(1, coll.find({$text: {$search: "testing", $language: "en"}}).itcount());

// SERVER-78238: index with a wildcard should not index fields with a dot inside or starting with $.
assert.commandWorked(coll.insert({"a.b": "ignored"}));
assert.commandWorked(coll.insert({"$personal": "ignored"}));
assert.eq(0, coll.find({$text: {$search: "ignored"}}).itcount());

// 11) Create a text index on a single field with a custom language override, insert a document,
// update the language of the document (so as to change the stemming), and verify that $text with
// the new language returns the document.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: "text"}, {language_override: "idioma"}));
assert.commandWorked(coll.insert({a: "testing", idioma: "es"}));
assert.eq(1, coll.find({$text: {$search: "testing", $language: "es"}}).itcount());
assert.eq(0, coll.find({$text: {$search: "testing", $language: "en"}}).itcount());
assert.commandWorked(coll.update({}, {$set: {idioma: "en"}}));
assert.eq(0, coll.find({$text: {$search: "testing", $language: "es"}}).itcount());
assert.eq(1, coll.find({$text: {$search: "testing", $language: "en"}}).itcount());

// 12) Same as #11, but with a wildcard text index.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({"$**": "text"}, {language_override: "idioma"}));
assert.commandWorked(coll.insert({a: "testing", idioma: "es"}));
assert.eq(1, coll.find({$text: {$search: "testing", $language: "es"}}).itcount());
assert.eq(0, coll.find({$text: {$search: "testing", $language: "en"}}).itcount());
assert.commandWorked(coll.update({}, {$set: {idioma: "en"}}));
assert.eq(0, coll.find({$text: {$search: "testing", $language: "es"}}).itcount());
assert.eq(1, coll.find({$text: {$search: "testing", $language: "en"}}).itcount());
