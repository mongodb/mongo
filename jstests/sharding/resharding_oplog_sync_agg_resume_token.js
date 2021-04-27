/**
 * Test that the postBatchResumeToken field is only included for the oplog namespace when
 * $_requestReshardingResumeToken is specified for an aggregate command.
 *
 * @tags: [
 *   requires_fcv_47,
 * ]
 */
(function() {
"use strict";

// Returns true if timestamp 'ts1' value is greater than timestamp 'ts2' value.
function timestampGreaterThan(ts1, ts2) {
    return ts1.getTime() > ts2.getTime() ||
        (ts1.getTime() == ts2.getTime() && ts1.getInc() > ts2.getInc());
}

var rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

// Insert documents to generate oplog entries.
let testDB = rst.getPrimary().getDB(dbName);
let testColl = testDB.foo;
for (let i = 0; i < 10; i++) {
    assert.commandWorked(testColl.insert({x: i}));
}

const localDb = rst.getPrimary().getDB("local");

// Run aggregation pipeline on oplog with $_requestReshardingResumeToken set when the pipeline can
// be optimized away.
const resEnabled = localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}],
    $_requestReshardingResumeToken: true,
    cursor: {batchSize: 1}
});

assert.commandWorked(resEnabled);
assert(resEnabled.cursor.hasOwnProperty("postBatchResumeToken"), resEnabled);
assert(resEnabled.cursor.postBatchResumeToken.hasOwnProperty("ts"), resEnabled);

// Ensure that postBatchResumeToken attribute is returned for getMore command.
const cursorId = resEnabled.cursor.id;
const resGetMore =
    assert.commandWorked(localDb.runCommand({getMore: cursorId, collection: "oplog.rs"}));

assert.commandWorked(resGetMore);
assert(resGetMore.cursor.hasOwnProperty("postBatchResumeToken"), resGetMore);
assert(resGetMore.cursor.postBatchResumeToken.hasOwnProperty("ts"), resGetMore);

// Run aggregation pipeline on oplog with $_requestReshardingResumeToken disabled.
const resDisabled = localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}],
    $_requestReshardingResumeToken: false,
    cursor: {}
});

assert.commandWorked(resDisabled);
assert(!resDisabled.cursor.hasOwnProperty("postBatchResumeToken"), resDisabled);

// Run aggregation pipeline on oplog with $_requestReshardingResumeToken unspecified and defaulting
// to disabled.
const resWithout = localDb.runCommand(
    {aggregate: "oplog.rs", pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}], cursor: {}});

assert.commandWorked(resWithout);
assert(!resWithout.cursor.hasOwnProperty("postBatchResumeToken"), resWithout);

// Run aggregation pipeline on non-oplog with $_requestReshardingResumeToken set.
const resNotOplog = localDb.runCommand(
    {aggregate: ns, pipeline: [{$limit: 100}], $_requestReshardingResumeToken: true, cursor: {}});

assert.commandFailedWithCode(resNotOplog,
                             ErrorCodes.FailedToParse,
                             "$_requestReshardingResumeToken set on non-oplog should fail");

// Run $changeStream on oplog with $_requestReshardingResumeToken set.
const resChangeStreamOnOplogWithRequestReshardingResumeToken = localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [{$changeStream: {}}],
    $_requestReshardingResumeToken: true,
    cursor: {}
});
assert.commandFailedWithCode(resChangeStreamOnOplogWithRequestReshardingResumeToken,
                             ErrorCodes.InvalidNamespace,
                             "$changeStream on oplog should fail");

// Run $changeStream with $_requestReshardingResumeToken set on non-oplog collection.
const resChangeStreamWithRequestReshardingResumeToken = testDB.runCommand({
    aggregate: collName,
    pipeline: [{$changeStream: {}}],
    $_requestReshardingResumeToken: true,
    cursor: {}
});
assert.commandFailedWithCode(resChangeStreamWithRequestReshardingResumeToken,
                             ErrorCodes.FailedToParse,
                             "$_requestReshardingResumeToken set with $changeStream should fail");

// Run aggregation pipeline on oplog with empty batch.
const resEmpty = localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}],
    $_requestReshardingResumeToken: true,
    cursor: {batchSize: 0}
});

assert.commandWorked(resEmpty);
assert(resEmpty.cursor.hasOwnProperty("postBatchResumeToken"), resEmpty);
assert(resEmpty.cursor.postBatchResumeToken.hasOwnProperty("ts"), resEmpty);
assert.eq(resEmpty.cursor.postBatchResumeToken.ts, new Timestamp(0, 0));

// Run aggregation pipeline on oplog with $_requestReshardingResumeToken set when the pipeline can
// not be optimized away.
const batchSize = 5;
let result = localDb.runCommand({
    aggregate: "oplog.rs",
    // The $_internalInhibitOptimization prevents the pipeline from being optimized away as a simple
    // plan executor. This is necessary to force the pipeline to be evaluated using
    // PlanExecutorPipeline.
    pipeline: [
        {$match: {ts: {$gte: Timestamp(0, 0)}, "o.x": {$lt: 8}}},
        {$_internalInhibitOptimization: {}}
    ],
    $_requestReshardingResumeToken: true,
    cursor: {batchSize: batchSize}
});
assert.commandWorked(result);
assert(result.cursor.hasOwnProperty("postBatchResumeToken"), result);
assert(result.cursor.postBatchResumeToken.hasOwnProperty("ts"), result);
assert.eq(result.cursor.firstBatch.length, batchSize, result);

// Verify that the postBatchResumeToken is equal to the 'ts' of the last record.
assert.eq(result.cursor.postBatchResumeToken.ts,
          result.cursor.firstBatch[result.cursor.firstBatch.length - 1].ts);

// Ensure that postBatchResumeToken attribute is returned for getMore command by reading the second
// batch. There are not enough matching documents left in the oplog to fill an entire batch, so we
// expect the PBRT to exceed the ts of the final entry.
result =
    assert.commandWorked(localDb.runCommand({getMore: result.cursor.id, collection: "oplog.rs"}));
assert(result.cursor.hasOwnProperty("postBatchResumeToken"), result);
assert(result.cursor.postBatchResumeToken.hasOwnProperty("ts"), result);
let resultsBatch = result.cursor.nextBatch;
assert(resultsBatch.length < batchSize, result);

// Verify that the postBatchResumeToken is greater than the 'ts' of the last read record since
// the documents in the rest of the collection do not match the filter "o.x": {$lt: 8}.
assert(timestampGreaterThan(result.cursor.postBatchResumeToken.ts,
                            resultsBatch[resultsBatch.length - 1].ts),
       "postBatchResumeToken value should be greater than 'ts' of the last record");

// Read all records in one batch.
result = localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [
        {$match: {ts: {$gte: Timestamp(0, 0)}, "o.x": {$lt: 2}}},
        {$_internalInhibitOptimization: {}}
    ],
    $_requestReshardingResumeToken: true,
    cursor: {}
});
assert.commandWorked(result);
assert(result.cursor.hasOwnProperty("postBatchResumeToken"), result);
assert(result.cursor.postBatchResumeToken.hasOwnProperty("ts"), result);
resultsBatch = result.cursor.firstBatch;

// Verify that the postBatchResumeToken is greater than the 'ts' of the last read record since
// the documents in the rest of the collection do not match the filter "o.x": {$lt: 2}.
assert(timestampGreaterThan(result.cursor.postBatchResumeToken.ts,
                            resultsBatch[resultsBatch.length - 1].ts),
       "postBatchResumeToken value should be greater than 'ts' of the last record");

// Run aggregation pipeline on oplog with batchSize: 0 when the pipeline can not be optimized away.
result = localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [
        {$match: {ts: {$gte: Timestamp(0, 0)}, "o.x": {$lt: 2}}},
        {$_internalInhibitOptimization: {}}
    ],
    $_requestReshardingResumeToken: true,
    cursor: {batchSize: 0}
});

assert.commandWorked(result);
assert(result.cursor.hasOwnProperty("postBatchResumeToken"), result);
assert(result.cursor.postBatchResumeToken.hasOwnProperty("ts"), result);
assert.eq(result.cursor.postBatchResumeToken.ts, new Timestamp(0, 0));

// Run aggregation pipeline on oplog with $_requestReshardingResumeToken set to false when the
// pipeline can not be optimized away.
result = localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}, {$_internalInhibitOptimization: {}}],
    $_requestReshardingResumeToken: false,
    cursor: {batchSize: 5}
});
assert.commandWorked(result);
assert(!result.cursor.hasOwnProperty("postBatchResumeToken"), result);
rst.stopSet();
})();
