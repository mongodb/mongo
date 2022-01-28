/**
 * Tests the behavior of the $_internalBoundedSort stage.
 * @tags: [
 *   requires_fcv_50,
 *   requires_timeseries,
 *   # Cannot insert into a time-series collection in a multi-document transaction.
 *   does_not_support_transactions,
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/analyze_plan.js');
load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.bucketUnpackWithSortEnabled(db.getMongo())) {
    jsTestLog("Skipping test because 'BucketUnpackWithSort' is disabled.");
    return;
}

const coll = db.timeseries_internal_bounded_sort;
const buckets = db['system.buckets.' + coll.getName()];
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 't', metaField: 'm'}}));
const bucketMaxSpanSeconds =
    db.getCollectionInfos({name: coll.getName()})[0].options.timeseries.bucketMaxSpanSeconds;

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

// Test sorting the whole collection.
{
    const naive = buckets
                      .aggregate([
                          unpackStage,
                          {$_internalInhibitOptimization: {}},
                          {$sort: {t: 1}},
                      ])
                      .toArray();
    assertSorted(naive);

    const opt = buckets
                    .aggregate([
                        {$sort: {'control.min.t': 1}},
                        unpackStage,
                        {
                            $_internalBoundedSort: {
                                sortKey: {t: 1},
                                bound: bucketMaxSpanSeconds,
                            }
                        },
                    ])
                    .toArray();
    assertSorted(opt);

    assert.eq(naive, opt);
}

// Test $sort + $limit.
{
    const naive = buckets
                      .aggregate([
                          unpackStage,
                          {$_internalInhibitOptimization: {}},
                          {$sort: {t: 1}},
                          {$limit: 100},
                      ])
                      .toArray();
    assertSorted(naive);
    assert.eq(100, naive.length);

    const opt = buckets
                    .aggregate([
                        {$sort: {'control.min.t': 1}},
                        unpackStage,
                        {
                            $_internalBoundedSort: {
                                sortKey: {t: 1},
                                bound: bucketMaxSpanSeconds,
                            }
                        },
                        {$limit: 100},
                    ])
                    .toArray();
    assertSorted(opt);
    assert.eq(100, opt.length);

    assert.eq(naive, opt);
}
})();
