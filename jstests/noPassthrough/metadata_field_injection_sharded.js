/**
 * Verifies that user documents with $-prefixed fields matching internal metadata names
 * are handled correctly on sharded clusters:
 *   1. Injected metadata-named fields are sanitized through merge sort.
 *   2. Stored metadata-named fields are sanitized when forced through merge sort.
 *   3. Stored metadata-named fields survive single-shard queries.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */

// Only include metadata field names that exist on v8.0 and that are still stripped by
// Document::toBsonStrippingMetadata. $sortKey is intentionally excluded on v8.0 — the
// legacy resharding cloner (makeRawPipeline, FCV < 7.2) injects it as a user field that
// must survive to the recipient's AsyncResultsMerger. Mongos's
// RouterStageRemoveMetadataFields still scrubs $sortKey from user-visible output.
// $stream and $changeStreamControlEvent were added post-v8.0 and are not stripped here.
const kMetadataFieldNames = [
    "$textScore",
    "$searchScore",
    "$randVal",
    "$dis",
    "$score",
    "$vectorSearchScore",
    "$pt",
    "$indexKey",
    "$searchScoreDetails",
    "$searchSortValues",
    "$searchHighlights",
    "$searchSequenceToken",
    "$scoreDetails",
];

// $replaceRoot injects a metadata-named field, $sort + $limit forces split-merge across shards,
// exercising toBsonWithMetaData() where metadata-named user fields are stripped.
function assertInjectedFieldSanitized(coll, fieldName) {
    const results =
        coll.aggregate([
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
    assert.eq(first.val, null, `${fieldName} should be stripped, got: ${tojson(first)}`);
    assert.eq(second.val, null, `${fieldName} should be stripped, got: ${tojson(second)}`);
}

// Documents with metadata-named fields are inserted on both shards (straddle split point 0).
// $sort + $limit forces merge sort, sending them through toBsonWithMetaData() which strips the
// fields. Pre-fix, loadLazyMetadata() would misinterpret these as real metadata.
function assertStoredFieldSanitizedThroughMergeSort(coll, lowId, highId, fieldName) {
    assert.commandWorked(coll.insert({_id: lowId, [fieldName]: "stored"}));
    assert.commandWorked(coll.insert({_id: highId, [fieldName]: "stored"}));
    const results = coll.aggregate([
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
    assert.eq(low.val, null, `${fieldName} should be stripped, got: ${tojson(low)}`);
    assert.eq(high.val, null, `${fieldName} should be stripped, got: ${tojson(high)}`);
}

// Both documents are on the same shard, so the query is routed without merge sort. The field
// never goes through toBsonWithMetaData() and survives intact.
function assertStoredFieldSurvivesSingleShard(coll, id1, id2, fieldName) {
    assert.commandWorked(coll.insert({_id: id1, [fieldName]: "stored"}));
    assert.commandWorked(coll.insert({_id: id2, [fieldName]: "stored"}));
    const results = coll.aggregate([
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
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: splitPoint}, to: st.shard1.shardName}));
}

// Split point 0: _id < 0 on shard0, _id >= 0 on shard1.
const st = new ShardingTest({shards: 2});
const db = st.s.getDB("test");
assert.commandWorked(
    st.s.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName}));

const injectColl = db.metadata_inject;
setupShardedCollection(st, "test.metadata_inject", 0);
const bulk = injectColl.initializeUnorderedBulkOp();
for (let i = -5; i < 5; i++) {
    bulk.insert({_id: i, x: i});
}
assert.commandWorked(bulk.execute());

const storedColl = db.metadata_stored;
setupShardedCollection(st, "test.metadata_stored", 0);

// 1. Injected metadata-named fields are sanitized through merge sort.
for (const fieldName of kMetadataFieldNames) {
    assertInjectedFieldSanitized(injectColl, fieldName);
}

// 2. Stored metadata-named fields are sanitized when forced through merge sort.
for (const [i, field] of kMetadataFieldNames.entries()) {
    assertStoredFieldSanitizedThroughMergeSort(storedColl, -(i + 1), i + 1, field);
}

// 3. Stored metadata-named fields survive single-shard queries.
for (const [i, field] of kMetadataFieldNames.entries()) {
    assertStoredFieldSurvivesSingleShard(storedColl, i + 100, i + 200, field);
}

// 4. $sortKey user-query expectations.
// $sortKey is exempt from Document::toBsonStrippingMetadata so the legacy resharding cloner
// (makeRawPipeline, FCV < 7.2) can deliver its injected sort key to the recipient's
// AsyncResultsMerger. User surface still needs to be tight:
//   (a) mongos's RouterStageRemoveMetadataFields scrubs $sortKey from any cross-shard output,
//   (b) cross-shard $sort with a user-injected $sortKey fails deterministically on mongos
//       rather than silently misordering results.

// (a) Non-sorting cross-shard aggregation: mongos strips the field on its way out.
const sortKeyNoSortResults = injectColl.aggregate([{
    $replaceRoot: {newRoot: {$mergeObjects: ["$$ROOT", {$literal: {"$sortKey": "injected"}}]}},
}]).toArray();
assert.eq(sortKeyNoSortResults.length, 10,
          `Expected 10 results, got: ${tojson(sortKeyNoSortResults)}`);
for (const r of sortKeyNoSortResults) {
    assert(!("$sortKey" in r),
           `$sortKey should be stripped by mongos, got: ${tojson(r)}`);
}

// (b) Cross-shard $sort with user-injected $sortKey: ARM sees the user field first on the
// wire and rejects it with ErrorCodes.InternalError ("Field '$sortKey' was not of type Object").
const sortKeyErr = assert.throws(() => injectColl.aggregate([
    {
        $replaceRoot: {
            newRoot: {$mergeObjects: ["$$ROOT", {$literal: {"$sortKey": "injected"}}]},
        },
    },
    {$sort: {_id: 1}},
    {$limit: 2},
]).toArray());
assert.eq(sortKeyErr.code,
          ErrorCodes.InternalError,
          `expected ARM $sortKey InternalError, got: ${tojson(sortKeyErr)}`);
assert(sortKeyErr.message.includes("$sortKey"),
       `expected error message to mention $sortKey, got: ${sortKeyErr.message}`);

st.stop();
