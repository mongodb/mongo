/**
 * Confirms that queries which scan multiple paths in a single wildcard index do not return
 * duplicate documents. For example, the object {a: {b: 1, c: 1}} will generate $** index keys with
 * paths "a.b" and "a.c". An index scan that covers both paths should deduplicate the documents
 * scanned and return only a single object.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");  // For "FeatureFlagUtil"

const coll = db.wildcard_index_dedup;
coll.drop();

assert.commandWorked(coll.createIndex({"$**": 1}));

// TODO SERVER-68303: Remove the feature flag and update corresponding tests.
const allowCompoundWildcardIndexes =
    FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), "CompoundWildcardIndexes");

const compoundKeyPattern = {
    "a.$**": 1,
    post: 1
};
if (allowCompoundWildcardIndexes) {
    assert.commandWorked(coll.createIndex(compoundKeyPattern));
}

assert.commandWorked(
    coll.insert({a: {b: 1, c: {f: 1, g: 1}, h: [1, 2, 3]}, d: {e: [1, 2, 3]}, post: 1}));

// An $exists that matches multiple $** index paths from nested objects does not return
// duplicates of the same object.
assert.eq(1, coll.find({a: {$exists: true}}).hint({"$**": 1}).itcount());

// An $exists that matches multiple $** index paths from nested array does not return
// duplicates of the same object.
assert.eq(1, coll.find({d: {$exists: true}}).hint({"$**": 1}).itcount());

// An $exists with dotted path that matches multiple $** index paths from nested objects
// does not return duplicates of the same object.
assert.eq(1, coll.find({"a.c": {$exists: true}}).hint({"$**": 1}).itcount());

if (allowCompoundWildcardIndexes) {
    // Test compound wildcard indexes do not return duplicates.
    assert.eq(1, coll.find({"a.c": {$exists: true}, post: 1}).hint(compoundKeyPattern).itcount());
    assert.eq(1, coll.find({"a.h": {$exists: true}, post: 1}).hint(compoundKeyPattern).itcount());
}
})();
