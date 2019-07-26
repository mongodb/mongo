/**
 * Confirms that queries which scan multiple paths in a single wildcard index do not return
 * duplicate documents. For example, the object {a: {b: 1, c: 1}} will generate $** index keys with
 * paths "a.b" and "a.c". An index scan that covers both paths should deduplicate the documents
 * scanned and return only a single object.
 */
(function() {
"use strict";

const coll = db.wildcard_index_dedup;
coll.drop();

assert.commandWorked(coll.createIndex({"$**": 1}));

assert.commandWorked(coll.insert({a: {b: 1, c: {f: 1, g: 1}}, d: {e: [1, 2, 3]}}));

// An $exists that matches multiple $** index paths from nested objects does not return
// duplicates of the same object.
assert.eq(1, coll.find({a: {$exists: true}}).hint({"$**": 1}).itcount());

// An $exists that matches multiple $** index paths from nested array does not return
// duplicates of the same object.
assert.eq(1, coll.find({d: {$exists: true}}).hint({"$**": 1}).itcount());

// An $exists with dotted path that matches multiple $** index paths from nested objects
// does not return duplicates of the same object.
assert.eq(1, coll.find({"a.c": {$exists: true}}).hint({"$**": 1}).itcount());
})();