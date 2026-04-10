/**
 * Ensure that in trivial cases when batch size is equal to limit, an open cursor is not returned to the client.
 * @tags: [
 *   assumes_no_implicit_cursor_exhaustion,
 *   requires_fcv_90,
 *   requires_capped,
 *   # Time-series buckets processing may have different cursor behavior.
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db[jsTestName()];
coll.drop();

for (let i = 0; i < 100; ++i) {
    assert.commandWorked(coll.insert({index: i}));
}

function assertSingleBatch({cursor, batchSize, expectSingleBatch}) {
    assert.eq(cursor.objsLeftInBatch(), batchSize);
    assert.eq(cursor.isClosed(), expectSingleBatch);
}

function testSingleBatchFind({query, sort, limit, batchSize, expectSingleBatch}) {
    const cursor = coll.find(query);
    if (sort !== undefined) {
        cursor.sort(sort);
    }
    if (limit !== undefined) {
        cursor.limit(limit);
    }
    cursor.batchSize(batchSize);
    assertSingleBatch({cursor, batchSize, expectSingleBatch});
}

function testSingleBatchAggregate({pipeline, batchSize, expectSingleBatch}) {
    const cursor = coll.aggregate(pipeline, {cursor: {batchSize: batchSize}});
    assertSingleBatch({cursor, batchSize, expectSingleBatch});
}

testSingleBatchFind({query: {index: {$gte: 50}}, limit: 10, batchSize: 10, expectSingleBatch: true});
testSingleBatchAggregate({
    pipeline: [{$match: {index: {$gte: 50}}}, {$limit: 10}],
    batchSize: 10,
    expectSingleBatch: true,
});

testSingleBatchFind({query: {index: {$gte: 50}}, sort: {index: 1}, limit: 10, batchSize: 10, expectSingleBatch: true});
testSingleBatchAggregate({
    pipeline: [{$match: {index: {$gte: 50}}}, {$sort: {index: 1}}, {$limit: 10}],
    batchSize: 10,
    expectSingleBatch: true,
});

testSingleBatchFind({query: {index: {$gte: 50}}, batchSize: 10, expectSingleBatch: false});

// There are exactly 10 matching documents, but there is no limit, so on mongod we must finish
// the scan to be sure. On sharded clusters his case is not stable, because is some cases mongos
// may pull the whole output from mongod and know that we are finished.
if (!FixtureHelpers.isSharded(coll)) {
    testSingleBatchAggregate({pipeline: [{$match: {index: {$lt: 10}}}], batchSize: 10, expectSingleBatch: false});
}

// $_internalInhibitOptimization prevents the optimizer from reordering stages and forces the
// agg pipeline execution path (rather than SBE).
const inhibit = {$_internalInhibitOptimization: {}};

// $group after $limit: group consumes all input and then drains, reporting EOF.
testSingleBatchAggregate({
    pipeline: [
        {$match: {index: {$gte: 50}}},
        {$sort: {index: 1}},
        {$limit: 10},
        inhibit,
        {$group: {_id: "$index", count: {$sum: 1}}},
    ],
    batchSize: 10,
    expectSingleBatch: true,
});

// Streaming stages after $limit propagate isEOF(), so the cursor should still close early.

// $limit followed by $project.
testSingleBatchAggregate({
    pipeline: [{$match: {index: {$gte: 50}}}, {$limit: 10}, inhibit, {$project: {index: 1}}],
    batchSize: 10,
    expectSingleBatch: true,
});

// $limit followed by $addFields.
testSingleBatchAggregate({
    pipeline: [{$match: {index: {$gte: 50}}}, {$limit: 10}, inhibit, {$addFields: {extra: 1}}],
    batchSize: 10,
    expectSingleBatch: true,
});

// $limit followed by $match (that passes all documents through).
testSingleBatchAggregate({
    pipeline: [{$match: {index: {$gte: 50}}}, {$limit: 10}, inhibit, {$match: {index: {$gte: 0}}}],
    batchSize: 10,
    expectSingleBatch: true,
});

// $limit followed by $skip 0 (pass-through skip).
testSingleBatchAggregate({
    pipeline: [{$match: {index: {$gte: 50}}}, {$limit: 10}, inhibit, {$skip: 0}],
    batchSize: 10,
    expectSingleBatch: true,
});

// $limit followed by $unwind on a non-array field (1:1 mapping).
testSingleBatchAggregate({
    pipeline: [{$match: {index: {$gte: 50}}}, {$limit: 10}, inhibit, {$unwind: "$index"}],
    batchSize: 10,
    expectSingleBatch: true,
});

// Chain of streaming stages after $limit.
testSingleBatchAggregate({
    pipeline: [
        {$match: {index: {$gte: 50}}},
        {$limit: 10},
        inhibit,
        {$addFields: {extra: 1}},
        {$project: {index: 1, extra: 1}},
        {$match: {index: {$gte: 0}}},
    ],
    batchSize: 10,
    expectSingleBatch: true,
});

// $limit followed by $skip that reduces the result count. 15 - 5 = 10 output documents.
testSingleBatchAggregate({
    pipeline: [{$match: {index: {$gte: 50}}}, {$limit: 15}, inhibit, {$skip: 5}],
    batchSize: 10,
    expectSingleBatch: true,
});

// $sort after $limit without a limit on the sort itself: the eager EOF check only fires when
// the sort stage has its own limit, matching Classic and SBE behavior.
testSingleBatchAggregate({
    pipeline: [{$match: {index: {$gte: 50}}}, {$limit: 10}, inhibit, {$sort: {index: -1}}],
    batchSize: 10,
    expectSingleBatch: false,
});

// $sort followed by $limit: the optimizer absorbs $limit into $sort, giving the sort stage
// its own limit. The eager EOF check fires after draining, so the cursor closes early.
testSingleBatchAggregate({
    pipeline: [{$match: {index: {$gte: 50}}}, inhibit, {$sort: {index: -1}}, {$limit: 10}],
    batchSize: 10,
    expectSingleBatch: true,
});

// Tailable cursors on capped collections should not be closed even when limit == batchSize,
// because they must remain open to await new data.
const cappedColl = db[jsTestName() + "_capped"];
cappedColl.drop();
assert.commandWorked(db.createCollection(cappedColl.getName(), {capped: true, size: 1024 * 1024}));
for (let i = 0; i < 100; ++i) {
    assert.commandWorked(cappedColl.insert({index: i}));
}

const tailableCursor = cappedColl
    .find({index: {$gte: 50}})
    .tailable()
    .limit(10)
    .batchSize(10);
assertSingleBatch({cursor: tailableCursor, batchSize: 10, expectSingleBatch: false});
