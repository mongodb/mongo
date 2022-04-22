/**
 * Tests the behavior of the $_internalBoundedSort stage with spilling to disk.
 * @tags: [
 *   requires_fcv_60,
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/analyze_plan.js');
load("jstests/core/timeseries/libs/timeseries.js");

const kSmallMemoryLimit = 1024;
const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryMaxBlockingSortMemoryUsageBytes: kSmallMemoryLimit,
        featureFlagBucketUnpackWithSort: true
    }
});

const dbName = jsTestName();
const testDB = conn.getDB(dbName);

const coll = testDB.timeseries_internal_bounded_sort;
const buckets = testDB['system.buckets.' + coll.getName()];
coll.drop();
assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: 't', metaField: 'm'}}));

// Insert some data.
{
    const numBatches = 10;
    const batchSize = 1000;
    const start = new Date();
    const intervalMillis = 1000;  // 1 second
    for (let i = 0; i < numBatches; ++i) {
        const batch = Array.from(
            {length: batchSize},
            (_, j) =>
                ({t: new Date(+start + i * batchSize * intervalMillis + j * intervalMillis)}));
        assert.commandWorked(coll.insert(batch));
        print(`Inserted ${i + 1} of ${numBatches} batches`);
    }
    assert.gt(buckets.aggregate([{$count: 'n'}]).next().n, 1, 'Expected more than one bucket');
}
// Create an index: we'll need this to scan the buckets in time order.
// TODO SERVER-60824 use the $natural / _id index instead.
assert.commandWorked(coll.createIndex({t: 1}));

const unpackStage = getAggPlanStage(coll.explain().aggregate(), '$_internalUnpackBucket');

function assertSorted(result) {
    let prev = {t: -Infinity};
    for (const doc of result) {
        assert.lte(+prev.t, +doc.t, 'Found two docs not in time order: ' + tojson({prev, doc}));
        prev = doc;
    }
}

// Test that memory limit would be hit by both implementations, and that both will error out if we
// don't enable disk use.
{
    // buckets.aggregate(...) uses assert.commandWorked internally, so we must use runCommand here
    // for error checking.
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: buckets.getName(),
        pipeline: [
            unpackStage,
            {$_internalInhibitOptimization: {}},
            {$sort: {t: 1}},
        ],
        cursor: {},
        allowDiskUse: false
    }),
                                 ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);

    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: buckets.getName(),
        pipeline: [
            {$sort: {'control.min.t': 1}},
            unpackStage,
            {
                $_internalBoundedSort: {
                    sortKey: {t: 1},
                    bound: {base: "min"},
                }
            },
        ],
        cursor: {},
        allowDiskUse: false
    }),
                                 ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
}

// Test sorting the whole collection.
{
    const naive = buckets
                      .aggregate(
                          [
                              unpackStage,
                              {$_internalInhibitOptimization: {}},
                              {$sort: {t: 1}},
                          ],
                          {allowDiskUse: true})
                      .toArray();
    assertSorted(naive);

    const pipeline = [
        {$sort: {'control.min.t': 1}},
        unpackStage,
        {
            $_internalBoundedSort: {
                sortKey: {t: 1},
                bound: {base: "min"},
            }
        },
    ];
    const opt = buckets.aggregate(pipeline, {allowDiskUse: true}).toArray();
    assertSorted(opt);

    assert.eq(naive, opt);

    // Let's make sure the execution stats make sense.
    const stats =
        getAggPlanStage(buckets.explain("executionStats").aggregate(pipeline, {allowDiskUse: true}),
                        '$_internalBoundedSort');
    assert.eq(stats.usedDisk, true);

    // We know each doc should have at least 8 bytes for time in both key and document.
    const docSize = stats.totalDataSizeSortedBytesEstimate / stats.nReturned;
    assert.gte(docSize, 16);
    const docsToTriggerSpill = Math.ceil(kSmallMemoryLimit / docSize);

    // We know we'll spill if we can't store all the docs from a single bucket within the memory
    // limit, so let's ensure that the total spills are at least what we'd expect if none of the
    // buckets overlap.
    const docsPerBucket = Math.floor(stats.nReturned / buckets.count());
    const spillsPerBucket = Math.floor(docsPerBucket / docsToTriggerSpill);
    assert.gt(spillsPerBucket, 0);
    assert.gte(stats.spills, buckets.count() * spillsPerBucket);
}

// Test $sort + $limit.
{
    const naive = buckets
                      .aggregate(
                          [
                              unpackStage,
                              {$_internalInhibitOptimization: {}},
                              {$sort: {t: 1}},
                              {$limit: 100},
                          ],
                          {allowDiskUse: true})
                      .toArray();
    assertSorted(naive);
    assert.eq(100, naive.length);

    const opt =
        buckets
            .aggregate(
                [
                    {$sort: {'control.min.t': 1}},
                    unpackStage,
                    {$_internalBoundedSort: {sortKey: {t: 1}, bound: {base: "min"}, limit: 100}}
                ],
                {allowDiskUse: true})
            .toArray();
    assertSorted(opt);
    assert.eq(100, opt.length);

    assert.eq(naive, opt);
}

MongoRunner.stopMongod(conn);
})();
