/**
 * Tests that a document generating no index keys does not mark the index multikey, even when the
 * paths it traversed on the way to the indexed field included an array. Such a document puts
 * nothing in the index, so nothing reachable through the index is multikey.
 *
 * @tags: [
 *   assumes_read_concern_local,
 *   requires_fcv_91,
 *   uses_explain,
 * ]
 */
import {getPlanStages} from "jstests/libs/query/analyze_plan.js";

function isMultikey(coll, keyPattern) {
    const explain = assert.commandWorked(coll.find().hint(keyPattern).explain());
    const ixscans = getPlanStages(explain, "IXSCAN");
    assert.gt(ixscans.length, 0, "expected an IXSCAN", {explain});
    // Multikey metadata is shard-local, so shards without the document may remain non-multikey.
    return ixscans.some((ixscan) => ixscan.isMultiKey);
}

// A sparse index skips the null key a missing leaf field would otherwise produce, so this document
// generates no keys at all -- while still traversing the array at `a`.
{
    const coll = db.getCollection(jsTestName() + "_sparse");
    coll.drop();
    assert.commandWorked(coll.createIndex({"a.b": 1}, {sparse: true}));
    assert.commandWorked(coll.insert({_id: 0, a: [{c: 1}, {c: 2}]}));
    assert.eq(
        isMultikey(coll, {"a.b": 1}),
        false,
        "document generating no keys must not mark the index multikey",
    );
}

// The same document in a non-sparse index does generate a key, and so does mark it multikey.
{
    const coll = db.getCollection(jsTestName() + "_nonSparse");
    coll.drop();
    assert.commandWorked(coll.createIndex({"a.b": 1}));
    assert.commandWorked(coll.insert({_id: 0, a: [{c: 1}, {c: 2}]}));
    assert.eq(
        isMultikey(coll, {"a.b": 1}),
        true,
        "document generating a key from an array must mark the index multikey",
    );
}

// An empty array is indexed as `undefined`, so it generates a key and stays multikey even when the
// index is sparse.
{
    const coll = db.getCollection(jsTestName() + "_emptyArray");
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}, {sparse: true}));
    assert.commandWorked(coll.insert({_id: 0, a: []}));
    assert.eq(
        isMultikey(coll, {a: 1}),
        true,
        "an empty array generates a key and must mark the index multikey",
    );
}
