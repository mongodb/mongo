/**
 * Tests the behavior of the $_internalBoundedSort stage.
 * @tags: [
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
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

// Create a descending time index to allow sorting by control.max.t
assert.commandWorked(coll.createIndex({t: -1}));

// Insert some data.
{
    const numBatches = 10;
    const batchSize = 1000;
    const intervalMillis = 100000;  // 100 seconds
    const batchOffset = Math.floor(intervalMillis / (numBatches + 1));
    const start = new Date();
    for (let i = 0; i < numBatches; ++i) {
        const batch =
            Array.from({length: batchSize},
                       (_, j) => ({t: new Date(+start + i * batchOffset + j * intervalMillis)}));
        assert.commandWorked(coll.insert(batch));
        print(`Inserted ${i + 1} of ${numBatches} batches`);
    }
    assert.gt(buckets.aggregate([{$count: 'n'}]).next().n, 1, 'Expected more than one bucket');

    TimeseriesTest.ensureDataIsDistributedIfSharded(
        coll, new Date(+start + ((batchSize / 2) * intervalMillis)));
}

const unpackStage = getAggPlanStages(coll.explain().aggregate(), '$_internalUnpackBucket')[0];

function assertSorted(result, ascending) {
    let prev = ascending ? {t: -Infinity} : {t: Infinity};
    for (const doc of result) {
        if (ascending) {
            assert.lte(+prev.t,
                       +doc.t,
                       'Found two docs not in ascending time order: ' + tojson({prev, doc}));
        } else {
            assert.gte(+prev.t,
                       +doc.t,
                       'Found two docs not in descending time order: ' + tojson({prev, doc}));
        }

        prev = doc;
    }
}

function checkAgainstReference(reference, pipeline, hint, sortOrder) {
    const opt = coll.aggregate(pipeline, {hint}).toArray();
    assertSorted(opt, sortOrder);
    assert.eq(reference, opt);

    const plan = coll.explain({}).aggregate(pipeline, {hint});
    const stages = getAggPlanStages(plan, "$_internalBoundedSort");
    assert.neq(null, stages, plan);
    assert.lte(1, stages.length, plan);
}

function runTest(ascending) {
    // Test sorting the whole collection
    {
        const reference = buckets
                              .aggregate([
                                  unpackStage,
                                  {$_internalInhibitOptimization: {}},
                                  {$sort: {t: ascending ? 1 : -1}},
                              ])
                              .toArray();
        assertSorted(reference, ascending);

        // Check plan using control.min.t
        checkAgainstReference(reference,
                              [
                                  {$sort: {t: ascending ? 1 : -1}},
                              ],
                              {"$natural": ascending ? 1 : -1},
                              ascending);

        // Check plan using control.max.t
        if (ascending) {
            const opt = coll.aggregate(
                                [
                                    {$sort: {t: 1}},
                                ],
                                {hint: {t: -1}})
                            .toArray();
            assertSorted(opt, ascending);
            assert.eq(reference, opt);
        } else {
            checkAgainstReference(reference,
                                  [
                                      {$sort: {t: ascending ? 1 : -1}},
                                  ],
                                  {"t": -1},
                                  ascending);
        }
    }

    // Test $sort + $limit.
    {
        const naive = buckets
                          .aggregate([
                              unpackStage,
                              {$_internalInhibitOptimization: {}},
                              {$sort: {t: ascending ? 1 : -1}},
                              {$limit: 100},
                          ])
                          .toArray();
        assertSorted(naive, ascending);
        assert.eq(100, naive.length);

        const optFromMin =
            buckets
                .aggregate([
                    {$sort: {'control.min.t': ascending ? 1 : -1}},
                    unpackStage,
                    {
                        $_internalBoundedSort: {
                            sortKey: {t: ascending ? 1 : -1},
                            bound: ascending ? {base: "min"}
                                             : {base: "min", offsetSeconds: bucketMaxSpanSeconds},
                            limit: 100
                        }
                    },
                ])
                .toArray();
        assertSorted(optFromMin, ascending);
        assert.eq(100, optFromMin.length);
        assert.eq(naive, optFromMin);

        const optFromMax =
            buckets
                .aggregate([
                    {$sort: {'control.max.t': ascending ? 1 : -1}},
                    unpackStage,
                    {
                        $_internalBoundedSort: {
                            sortKey: {t: ascending ? 1 : -1},
                            bound: ascending ? {base: "max", offsetSeconds: -bucketMaxSpanSeconds}
                                             : {base: "max"},
                            limit: 100
                        }
                    }
                ])
                .toArray();
        assertSorted(optFromMax, ascending);
        assert.eq(100, optFromMax.length);
        assert.eq(naive, optFromMax);
    }
}

runTest(true);   // ascending
runTest(false);  // descending
})();
