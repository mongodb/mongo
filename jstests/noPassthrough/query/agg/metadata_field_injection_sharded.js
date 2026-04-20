/**
 * Verifies that user documents with $-prefixed fields matching internal metadata names
 * are handled correctly on sharded clusters:
 *   1. Injected metadata-named fields are sanitized through merge sort.
 *   2. Stored metadata-named fields are sanitized when forced through merge sort.
 *   3. Stored metadata-named fields survive single-shard queries.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kMetadataFieldNames = [
    "$textScore",
    "$searchScore",
    "$randVal",
    "$dis",
    "$score",
    "$vectorSearchScore",
    "$pt",
    "$sortKey",
    "$indexKey",
    "$searchScoreDetails",
    "$searchSortValues",
    "$searchHighlights",
    "$searchSequenceToken",
    "$scoreDetails",
    "$stream",
    "$changeStreamControlEvent",
];

// $replaceRoot injects a metadata-named field, $sort + $limit forces split-merge across shards,
// exercising toBsonWithMetaData() where metadata-named user fields are stripped.
function assertInjectedFieldSanitized(coll, fieldName) {
    const results = coll
        .aggregate([
            {
                $replaceRoot: {
                    newRoot: {$mergeObjects: ["$$ROOT", {$literal: {[fieldName]: "injected"}}]},
                },
            },
            {$sort: {_id: 1}},
            {$limit: 2},
            {$project: {_id: 1, val: {$getField: {$const: fieldName}}}},
        ])
        .toArray();
    assert.eq(results.length, 2, `Expected 2 results for ${fieldName}`);
    const [first, second] = results;
    assert.eq(first._id, -5, `Expected _id=-5, got: ${tojson(first)}`);
    assert.eq(second._id, -4, `Expected _id=-4, got: ${tojson(second)}`);
    if (fieldName === "$sortKey") {
        // $sort sets real $sortKey metadata, so $getField returns the sort key.
        assert.neq(first.val, "injected", `$sortKey should be overwritten by real metadata`);
    } else {
        assert.eq(first.val, null, `${fieldName} should be stripped, got: ${tojson(first)}`);
        assert.eq(second.val, null, `${fieldName} should be stripped, got: ${tojson(second)}`);
    }
}

// Documents with metadata-named fields are inserted on both shards (straddle split point 0).
// $sort + $limit forces merge sort, sending them through toBsonWithMetaData() which strips the
// fields. Pre-fix, loadLazyMetadata() would misinterpret these as real metadata.
function assertStoredFieldSanitizedThroughMergeSort(coll, lowId, highId, fieldName) {
    assert.commandWorked(coll.insert({_id: lowId, [fieldName]: "stored"}));
    assert.commandWorked(coll.insert({_id: highId, [fieldName]: "stored"}));
    const results = coll
        .aggregate([
            {$match: {_id: {$in: [lowId, highId]}}},
            {$sort: {_id: 1}},
            {$limit: 2},
            {$project: {_id: 1, val: {$getField: {$const: fieldName}}}},
        ])
        .toArray();
    assert.eq(results.length, 2, `Expected 2 results for ${fieldName}`);
    const [low, high] = results;
    assert.eq(low._id, lowId, `Expected _id=${lowId}, got: ${tojson(low)}`);
    assert.eq(high._id, highId, `Expected _id=${highId}, got: ${tojson(high)}`);
    if (fieldName === "$sortKey") {
        // $sort sets real $sortKey metadata, so $getField returns the sort key.
        assert.neq(low.val, "stored");
    } else {
        assert.eq(low.val, null, `${fieldName} should be stripped, got: ${tojson(low)}`);
        assert.eq(high.val, null, `${fieldName} should be stripped, got: ${tojson(high)}`);
    }
}

// Both documents are on the same shard, so the query is routed without merge sort. The field
// never goes through toBsonWithMetaData() and survives intact.
function assertStoredFieldSurvivesSingleShard(coll, id1, id2, fieldName) {
    assert.commandWorked(coll.insert({_id: id1, [fieldName]: "stored"}));
    assert.commandWorked(coll.insert({_id: id2, [fieldName]: "stored"}));
    const results = coll
        .aggregate([
            {$match: {_id: {$in: [id1, id2]}}},
            {$sort: {_id: 1}},
            {$project: {_id: 1, val: {$getField: {$const: fieldName}}}},
        ])
        .toArray();
    assert.eq(results.length, 2, `Expected 2 results for ${fieldName}`);
    const [first, second] = results;
    assert.eq(first._id, id1, `Expected _id=${id1}, got: ${tojson(first)}`);
    assert.eq(second._id, id2, `Expected _id=${id2}, got: ${tojson(second)}`);
    assert.eq(first.val, "stored", `${fieldName} should survive, got: ${tojson(first)}`);
    assert.eq(second.val, "stored", `${fieldName} should survive, got: ${tojson(second)}`);
}

function setupShardedCollection(st, ns, splitPoint) {
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: splitPoint}}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: splitPoint}, to: st.shard1.shardName}));
}

describe("metadata field handling on sharded clusters", function () {
    // Split point 0: _id < 0 on shard0, _id >= 0 on shard1.
    before(function () {
        this.st = new ShardingTest({shards: 2});
        const db = this.st.s.getDB("test");
        assert.commandWorked(this.st.s.adminCommand({enableSharding: "test", primaryShard: this.st.shard0.shardName}));

        this.injectColl = db.metadata_inject;
        setupShardedCollection(this.st, "test.metadata_inject", 0);
        const bulk = this.injectColl.initializeUnorderedBulkOp();
        for (let i = -5; i < 5; i++) {
            bulk.insert({_id: i, x: i});
        }
        assert.commandWorked(bulk.execute());

        this.storedColl = db.metadata_stored;
        setupShardedCollection(this.st, "test.metadata_stored", 0);
    });

    after(function () {
        this.st.stop();
    });

    // 1. Injected metadata-named fields are sanitized through merge sort.
    for (const fieldName of kMetadataFieldNames) {
        it(`injected ${fieldName} is sanitized through merge sort`, function () {
            assertInjectedFieldSanitized(this.injectColl, fieldName);
        });
    }

    // 2. Stored metadata-named fields are sanitized when forced through merge sort.
    for (const [i, field] of kMetadataFieldNames.entries()) {
        it(`stored ${field} is sanitized through merge sort`, function () {
            assertStoredFieldSanitizedThroughMergeSort(this.storedColl, -(i + 1), i + 1, field);
        });
    }

    // 3. Stored metadata-named fields survive single-shard queries.
    for (const [i, field] of kMetadataFieldNames.entries()) {
        it(`stored ${field} survives single-shard query`, function () {
            assertStoredFieldSurvivesSingleShard(this.storedColl, i + 100, i + 200, field);
        });
    }
});
