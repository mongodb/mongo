/**
 * Tests commands(e.g. aggregate, create) that use pipeline stages not supported in API Version 1.
 *
 * Tests which create views aren't expected to work when collections are implicitly sharded.
 * @tags: [
 *   requires_fcv_49,
 *   uses_api_parameters,
 *   assumes_unsharded_collection,
 *   assumes_read_preference_unchanged,
 *   assumes_read_concern_unchanged
 * ]
 */

(function() {
"use strict";

const collName = "api_version_pipeline_stages";
const coll = db[collName];
coll.drop();
coll.insert({a: 1});

const unstablePipelines = [
    [{$collStats: {count: {}, latencyStats: {}}}],
    [{$currentOp: {}}],
    [{$indexStats: {}}],
    [{$listLocalSessions: {}}],
    [{$listSessions: {}}],
    [{$planCacheStats: {}}],
    [{$unionWith: {coll: "coll2", pipeline: [{$collStats: {latencyStats: {}}}]}}],
    [{$lookup: {from: "coll2", pipeline: [{$indexStats: {}}]}}],
    [{$lookup: {from: "coll2", _internalCollation: {locale: "simple"}}}],
    [{$facet: {field1: [], field2: [{$indexStats: {}}]}}],
];

function assertAggregateFailsWithAPIStrict(pipeline) {
    assert.commandFailedWithCode(db.runCommand({
        aggregate: collName,
        pipeline: pipeline,
        cursor: {},
        apiStrict: true,
        apiVersion: "1"
    }),
                                 ErrorCodes.APIStrictError);
}

for (let pipeline of unstablePipelines) {
    // Assert error thrown when running a pipeline with stages not in API Version 1.
    assertAggregateFailsWithAPIStrict(pipeline);

    // Assert error thrown when creating a view on a pipeline with stages not in API Version 1.
    assert.commandFailedWithCode(db.runCommand({
        create: 'api_version_pipeline_stages_should_fail',
        viewOn: collName,
        pipeline: pipeline,
        apiStrict: true,
        apiVersion: "1"
    }),
                                 ErrorCodes.APIStrictError);
}

// Test that $collStats is allowed in APIVersion 1, even with 'apiStrict: true', so long as the only
// parameter given is 'count'.
assertAggregateFailsWithAPIStrict([{$collStats: {latencyStats: {}}}]);
assertAggregateFailsWithAPIStrict([{$collStats: {latencyStats: {histograms: true}}}]);
assertAggregateFailsWithAPIStrict([{$collStats: {storageStats: {}}}]);
assertAggregateFailsWithAPIStrict([{$collStats: {queryExecStats: {}}}]);
assertAggregateFailsWithAPIStrict([{$collStats: {latencyStats: {}, queryExecStats: {}}}]);
assertAggregateFailsWithAPIStrict(
    [{$collStats: {latencyStats: {}, storageStats: {scale: 1024}, queryExecStats: {}}}]);

assert.doesNotThrow(
    () => db[collName].aggregate([{$collStats: {}}], {apiVersion: "1", apiStrict: true}));
assert.doesNotThrow(
    () => db[collName].aggregate([{$collStats: {count: {}}}], {apiVersion: "1", apiStrict: true}));

// Test that by running the aggregate command with $collStats + $group like our drivers do to
// compute the count, we get back a single result in the first batch - no getMore is required.
// This test is meant to mimic a drivers test and serve as a warning if we may be making a breaking
// change for the drivers.
const cmdResult = assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline: [{$collStats: {count: {}}}, {$group: {_id: 1, count: {$sum: "$count"}}}],
    cursor: {},
    apiVersion: "1",
    apiStrict: true
}));

assert.eq(cmdResult.cursor.id, 0, cmdResult);
assert.eq(cmdResult.cursor.firstBatch.length, 1, cmdResult);
assert.eq(cmdResult.cursor.firstBatch[0].count, 1, cmdResult);
})();
