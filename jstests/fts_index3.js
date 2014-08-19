// Test that updates to fields in a text-indexed document are correctly reflected in the text index.
load("jstests/libs/fts.js");
var coll = db.fts_index3;
var res;

// 1) Create a text index on a single field, insert a document, update the value of the field, and
// verify that searching with the new value returns the document.
coll.drop();
res = coll.ensureIndex({a: "text"});
assert.isnull(res);
coll.insert({a: "hello"});
assert(!db.getLastError());
assert.eq(1, coll.runCommand("text", {search: "hello"}).stats.n);
coll.update({}, {$set: {a: "world"}});
assert(!db.getLastError());
assert.eq(0, coll.runCommand("text", {search: "hello"}).stats.n);
assert.eq(1, coll.runCommand("text", {search: "world"}).stats.n);

// 2) Same as #1, but with a wildcard text index.
coll.drop();
res = coll.ensureIndex({"$**": "text"});
assert.isnull(res);
coll.insert({a: "hello"});
assert(!db.getLastError());
assert.eq(1, coll.runCommand("text", {search: "hello"}).stats.n);
coll.update({}, {$set: {a: "world"}});
assert(!db.getLastError());
assert.eq(0, coll.runCommand("text", {search: "hello"}).stats.n);
assert.eq(1, coll.runCommand("text", {search: "world"}).stats.n);

// 3) Create a compound text index with an index prefix, insert a document, update the value of the
// index prefix field, and verify that searching with the new value returns the document.
coll.drop();
res = coll.ensureIndex({a: 1, b: "text"});
assert.isnull(res);
coll.insert({a: 1, b: "hello"});
assert(!db.getLastError());
assert.eq(1, coll.runCommand("text", {search: "hello", filter: {a: 1}}).stats.n);
coll.update({}, {$set: {a: 2}});
assert(!db.getLastError());
assert.eq(0, coll.runCommand("text", {search: "hello", filter: {a: 1}}).stats.n);
assert.eq(1, coll.runCommand("text", {search: "hello", filter: {a: 2}}).stats.n);

// 4) Same as #3, but with a wildcard text index.
coll.drop();
res = coll.ensureIndex({a: 1, "$**": "text"});
assert.isnull(res);
coll.insert({a: 1, b: "hello"});
assert(!db.getLastError());
assert.eq(1, coll.runCommand("text", {search: "hello", filter: {a: 1}}).stats.n);
coll.update({}, {$set: {a: 2}});
assert(!db.getLastError());
assert.eq(0, coll.runCommand("text", {search: "hello", filter: {a: 1}}).stats.n);
assert.eq(1, coll.runCommand("text", {search: "hello", filter: {a: 2}}).stats.n);

// 5) Create a compound text index with an index suffix, insert a document, update the value of the
// index suffix field, and verify that searching with the new value returns the document.
coll.drop();
res = coll.ensureIndex({a: "text", b: 1});
assert.isnull(res);
coll.insert({a: "hello", b: 1});
assert(!db.getLastError());
assert.eq(1, coll.runCommand("text", {search: "hello", filter: {b: 1}}).stats.n);
coll.update({}, {$set: {b: 2}});
assert(!db.getLastError());
assert.eq(0, coll.runCommand("text", {search: "hello", filter: {b: 1}}).stats.n);
assert.eq(1, coll.runCommand("text", {search: "hello", filter: {b: 2}}).stats.n);

// 6) Same as #5, but with a wildcard text index.
coll.drop();
res = coll.ensureIndex({"$**": "text", b: 1});
assert.isnull(res);
coll.insert({a: "hello", b: 1});
assert(!db.getLastError());
assert.eq(1, coll.runCommand("text", {search: "hello", filter: {b: 1}}).stats.n);
coll.update({}, {$set: {b: 2}});
assert(!db.getLastError());
assert.eq(0, coll.runCommand("text", {search: "hello", filter: {b: 1}}).stats.n);
assert.eq(1, coll.runCommand("text", {search: "hello", filter: {b: 2}}).stats.n);

// 7) Create a text index on a single field, insert a document, update the language of the document
// (so as to change the stemming), and verify that searching with the new language returns the
// document.
coll.drop();
res = coll.ensureIndex({a: "text"});
assert.isnull(res);
coll.insert({a: "testing", language: "spanish"});
assert(!db.getLastError());
assert.eq(1, coll.runCommand("text", {search: "testing", language: "spanish"}).stats.n);
assert.eq(0, coll.runCommand("text", {search: "testing", language: "english"}).stats.n);
coll.update({}, {$set: {language: "english"}});
assert(!db.getLastError());
assert.eq(0, coll.runCommand("text", {search: "testing", language: "spanish"}).stats.n);
assert.eq(1, coll.runCommand("text", {search: "testing", language: "english"}).stats.n);

// 8) Same as #7, but with a wildcard text index.
coll.drop();
res = coll.ensureIndex({"$**": "text"});
assert.isnull(res);
coll.insert({a: "testing", language: "spanish"});
assert(!db.getLastError());
assert.eq(1, coll.runCommand("text", {search: "testing", language: "spanish"}).stats.n);
assert.eq(0, coll.runCommand("text", {search: "testing", language: "english"}).stats.n);
coll.update({}, {$set: {language: "english"}});
assert(!db.getLastError());
assert.eq(0, coll.runCommand("text", {search: "testing", language: "spanish"}).stats.n);
assert.eq(1, coll.runCommand("text", {search: "testing", language: "english"}).stats.n);

// 9) Create a text index on a single field with a custom language override, insert a document,
// update the language of the document (so as to change the stemming), and verify that searching
// with the new language returns the document.
coll.drop();
res = coll.ensureIndex({a: "text"}, {language_override: "idioma"});
assert.isnull(res);
coll.insert({a: "testing", idioma: "spanish"});
assert(!db.getLastError());
assert.eq(1, coll.runCommand("text", {search: "testing", language: "spanish"}).stats.n);
assert.eq(0, coll.runCommand("text", {search: "testing", language: "english"}).stats.n);
coll.update({}, {$set: {idioma: "english"}});
assert(!db.getLastError());
assert.eq(0, coll.runCommand("text", {search: "testing", language: "spanish"}).stats.n);
assert.eq(1, coll.runCommand("text", {search: "testing", language: "english"}).stats.n);

// 10) Same as #9, but with a wildcard text index.
coll.drop();
res = coll.ensureIndex({"$**": "text"}, {language_override: "idioma"});
assert.isnull(res);
coll.insert({a: "testing", idioma: "spanish"});
assert(!db.getLastError());
assert.eq(1, coll.runCommand("text", {search: "testing", language: "spanish"}).stats.n);
assert.eq(0, coll.runCommand("text", {search: "testing", language: "english"}).stats.n);
coll.update({}, {$set: {idioma: "english"}});
assert(!db.getLastError());
assert.eq(0, coll.runCommand("text", {search: "testing", language: "spanish"}).stats.n);
assert.eq(1, coll.runCommand("text", {search: "testing", language: "english"}).stats.n);
