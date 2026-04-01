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

// $group "hides" $limit, so we can't guarantee a single batch.
testSingleBatchAggregate({
    pipeline: [
        {$match: {index: {$gte: 50}}},
        {$sort: {index: 1}},
        {$limit: 10},
        {$group: {_id: "$index", count: {$sum: 1}}},
    ],
    batchSize: 10,
    expectSingleBatch: false,
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
