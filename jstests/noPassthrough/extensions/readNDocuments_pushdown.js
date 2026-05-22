/**
 * Tests $match and $project pushdown optimizations for the $readNDocuments test extension.
 *
 * The "applyMatchPushdown" rewrite rule on $produceIds folds a subsequent $match with an _id
 * lower-bound filter into the stage's startId, eliminating the $match from the pipeline.
 *
 * The "applyProjectPushdown" in-place rule suppresses "value" and "label" fields not present in
 * the downstream dep set. applyPipelineSuffixDependencies stores the getNeededFields() result and
 * the rule consumes it.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionsOptimizations,
 * ]
 */

import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

function runAllTests(coll) {
    // --------------------------$match Pushdown Test Cases--------------------------
    // Baseline: $readNDocuments produces _id, value, and label fields.
    {
        const result = coll.aggregate([{$readNDocuments: {numDocs: 5}}]).toArray();
        assert.eq(result.length, 5, "baseline: expected 5 docs");
        for (const doc of result) {
            assert.eq(doc.value, doc._id * 2, "baseline: wrong value", {doc});
            assert.eq(doc.label, `doc_${doc._id}`, "baseline: wrong label", {doc});
        }
    }

    // $match pushdown with $gte: only docs with _id >= 5 are returned.
    {
        const result = coll.aggregate([{$readNDocuments: {numDocs: 10}}, {$match: {_id: {$gte: 5}}}]).toArray();
        assert.eq(result.length, 5, "$match $gte pushdown: expected 5 docs");
        for (const doc of result) {
            assert.gte(doc._id, 5, "$match $gte pushdown: _id below lower bound", {doc});
        }
    }

    // $match pushdown with $gt: only docs with _id > 5 are returned.
    {
        const result = coll.aggregate([{$readNDocuments: {numDocs: 10}}, {$match: {_id: {$gt: 5}}}]).toArray();
        assert.eq(result.length, 4, "$match $gt pushdown: expected 4 docs");
        for (const doc of result) {
            assert.gt(doc._id, 5, "$match $gt pushdown: _id not above lower bound", {doc});
        }
    }

    // $match lower-bound beyond num docs outputted by $readNDocuments: produces zero docs.
    {
        const result = coll.aggregate([{$readNDocuments: {numDocs: 5}}, {$match: {_id: {$gte: 10}}}]).toArray();
        assert.eq(result.length, 0, "$match beyond numDocs: expected 0 docs");
    }

    // Non-absorbable $match: filter on "value" has no _id lower-bound so $match must not be erased.
    {
        const result = coll.aggregate([{$readNDocuments: {numDocs: 10}}, {$match: {value: {$gte: 10}}}]).toArray();
        // docs 0..4 have value 0..8 (< 10); docs 5..9 have value 10..18 (>= 10)
        assert.eq(result.length, 5, "non-absorbable $match: expected 5 docs");
        for (const doc of result) {
            assert.gte(doc.value, 10, "non-absorbable $match: value below bound", {doc});
        }
    }

    // --------------------------$project Pushdown Test Cases--------------------------
    // Neither "value" nor "label" should be produced when dep set = {_id}.
    {
        const result = coll.aggregate([{$readNDocuments: {numDocs: 5}}, {$project: {_id: 1}}]).toArray();
        assert.eq(result.length, 5, "$project pushdown: expected 5 docs");
        for (const doc of result) {
            assert(!doc.hasOwnProperty("value"), "$project pushdown: unexpected value field", {doc});
            assert(!doc.hasOwnProperty("label"), "$project pushdown: unexpected label field", {doc});
        }
    }

    // Only "label" should be suppressed when dep set = {_id, value}.
    {
        const result = coll.aggregate([{$readNDocuments: {numDocs: 5}}, {$project: {_id: 1, value: 1}}]).toArray();
        assert.eq(result.length, 5, "selective suppression (value): expected 5 docs");
        for (const doc of result) {
            assert(doc.hasOwnProperty("value"), "selective suppression (value): missing value", {doc});
            assert.eq(doc.value, doc._id * 2, "selective suppression (value): wrong value", {doc});
            assert(!doc.hasOwnProperty("label"), "selective suppression (value): unexpected label", {doc});
        }
    }

    // Only "value" should be suppressed when dep set = {_id, label}.
    {
        const result = coll.aggregate([{$readNDocuments: {numDocs: 5}}, {$project: {_id: 1, label: 1}}]).toArray();
        assert.eq(result.length, 5, "selective suppression (label): expected 5 docs");
        for (const doc of result) {
            assert(doc.hasOwnProperty("label"), "selective suppression (label): missing label", {doc});
            assert.eq(doc.label, `doc_${doc._id}`, "selective suppression (label): wrong label", {doc});
            assert(!doc.hasOwnProperty("value"), "selective suppression (label): unexpected value", {doc});
        }
    }
}

function runTests(conn, shardingTest) {
    const db = conn.getDB("test");
    const coll = db[jsTestName()];

    assert.commandWorked(
        coll.insertMany(Array.from({length: 10}, (_, i) => ({_id: i, value: i * 2, label: `doc_${i}`}))),
    );

    runAllTests(coll);

    if (shardingTest) {
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 5}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: 5},
                to: shardingTest.shard1.shardName,
            }),
        );

        runAllTests(coll);
    }
}

withExtensions({"libread_n_documents_mongo_extension.so": {}}, runTests, ["standalone", "sharded"], {shards: 2});
