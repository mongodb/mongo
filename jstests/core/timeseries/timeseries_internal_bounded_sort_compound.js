/**
 * Tests the behavior of the $_internalBoundedSort stage with a compound sort key.
 *
 * @tags: [
 *   requires_fcv_60,
 *   # Cannot insert into a time-series collection in a multi-document transaction.
 *   does_not_support_transactions,
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
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

const coll = db.timeseries_internal_bounded_sort_compound;
const buckets = db['system.buckets.' + coll.getName()];
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 't', metaField: 'm'}}));
const bucketMaxSpanSeconds =
    db.getCollectionInfos({name: coll.getName()})[0].options.timeseries.bucketMaxSpanSeconds;

// Insert some data.
{
    const numSeries = 5;
    const numBatchesPerSeries = 5;
    const batchSize = 120;  // 2 hours, if events are 1 minute apart
    const start = new Date();
    const intervalMillis = 1000 * 60;  // 1 minute

    for (let seriesIx = 0; seriesIx < numSeries; ++seriesIx) {
        for (let batchIx = 0; batchIx < numBatchesPerSeries; ++batchIx) {
            let batch = [];
            for (let eventIx = 0; eventIx < batchSize; ++eventIx) {
                const eventIxInSeries = batchIx * batchSize + eventIx;
                batch.push({
                    m: seriesIx,
                    t: new Date(+start + eventIxInSeries * intervalMillis),
                });
            }
            assert.commandWorked(coll.insert(batch));
        }
    }
    const expectedBucketsPerSeries = Math.floor((numBatchesPerSeries * batchSize * intervalMillis) /
                                                (bucketMaxSpanSeconds * 1000));
    assert.gte(buckets.aggregate([{$count: 'n'}]).next().n,
               expectedBucketsPerSeries * numSeries,
               `Expected at least ${expectedBucketsPerSeries} buckets per series ` +
                   `(${expectedBucketsPerSeries}*${numSeries} total)`);
}

const unpackStage = getAggPlanStage(coll.explain().aggregate(), '$_internalUnpackBucket');

function inOrder(prev, doc, sortSpec) {
    const signum = (v) => {
        if (v > 0)
            return +1;
        if (v < 0)
            return -1;
        return 0;
    };
    if (signum(doc.m - prev.m) == sortSpec.m) {
        // Good: the order on 'm' agrees with the sort spec.
        // 't' doesn't matter in this case.
        return true;
    } else if (doc.m === prev.m) {
        // Tie on 'm': compare 't'.
        if (signum(doc.t - prev.t) == sortSpec.t) {
            // Good: the order on 't' agrees with the sort spec.
            return true;
        } else if (doc.t === prev.t) {
            // Good: tie on both 'm' and 't'.
            return true;
        } else {
            // Bad: 'm' tied and 't' is in the wrong order.
            return false;
        }
    } else {
        // Bad: 'm' is in the wrong order.
        return false;
    }
}

function assertSorted(result, sortSpec) {
    assert.eq(['m', 't'], Object.keys(sortSpec), 'Expected a compound sort on {m: _, t: _}');
    assert.contains(sortSpec.m, [-1, +1]);
    assert.contains(sortSpec.t, [-1, +1]);

    if (result.length === 0)
        return;

    let prev = result[0];
    for (const doc of result.slice(1)) {
        assert(inOrder(prev, doc, sortSpec),
               'Found two docs not in ' + tojson(sortSpec) + ' order: ' + tojson({prev, doc}));

        prev = doc;
    }
}

function runTest(sortSpec) {
    assert.eq(['m', 't'], Object.keys(sortSpec), 'Expected a compound sort on {m: _, t: _}');
    assert.contains(sortSpec.m, [-1, +1]);
    assert.contains(sortSpec.t, [-1, +1]);

    // Test sorting the whole collection
    {
        const naiveQuery = [
            unpackStage,
            {$_internalInhibitOptimization: {}},
            {$sort: sortSpec},
        ];
        const naive = buckets.aggregate(naiveQuery).toArray();
        assertSorted(naive, sortSpec);

        const optFromMinQuery = [
            {$sort: {meta: sortSpec.m, 'control.min.t': sortSpec.t}},
            unpackStage,
            {
                $_internalBoundedSort: {
                    sortKey: sortSpec,
                    bound: sortSpec.t > 0 ? {base: "min"}
                                          : {base: "min", offsetSeconds: bucketMaxSpanSeconds}
                }
            },
        ];
        const optFromMin = buckets.aggregate(optFromMinQuery).toArray();
        assertSorted(optFromMin, sortSpec);
        assert.eq(naive, optFromMin);

        const optFromMaxQuery = [
            {$sort: {meta: sortSpec.m, 'control.max.t': sortSpec.t}},
            unpackStage,
            {
                $_internalBoundedSort: {
                    sortKey: sortSpec,
                    bound: sortSpec.t > 0 ? {base: "max", offsetSeconds: -bucketMaxSpanSeconds}
                                          : {base: "max"}
                }
            },
        ];
        const optFromMax = buckets.aggregate(optFromMaxQuery).toArray();
        assertSorted(optFromMax, sortSpec);
        assert.eq(naive, optFromMax);
    }

    // Test $sort + $limit.
    {
        const naiveQuery = [
            unpackStage,
            {$_internalInhibitOptimization: {}},
            {$sort: sortSpec},
            {$limit: 100},
        ];
        const naive = buckets.aggregate(naiveQuery).toArray();
        assertSorted(naive, sortSpec);
        assert.eq(100, naive.length);

        const optFromMinQuery = [
            {$sort: {meta: sortSpec.m, 'control.min.t': sortSpec.t}},
            unpackStage,
            {
                $_internalBoundedSort: {
                    sortKey: sortSpec,
                    bound: sortSpec.t > 0 ? {base: "min"}
                                          : {base: "min", offsetSeconds: bucketMaxSpanSeconds},
                    limit: 100
                }
            }
        ];
        const optFromMin = buckets.aggregate(optFromMinQuery).toArray();
        assertSorted(optFromMin, sortSpec);
        assert.eq(100, optFromMin.length);
        assert.eq(naive, optFromMin);

        const optFromMaxQuery = [
            {$sort: {meta: sortSpec.m, 'control.max.t': sortSpec.t}},
            unpackStage,
            {
                $_internalBoundedSort: {
                    sortKey: sortSpec,
                    bound: sortSpec.t > 0 ? {base: "max", offsetSeconds: -bucketMaxSpanSeconds}
                                          : {base: "max"},
                    limit: 100
                }
            }
        ];
        const optFromMax = buckets.aggregate(optFromMaxQuery).toArray();
        assertSorted(optFromMax, sortSpec);
        assert.eq(100, optFromMax.length);
        assert.eq(naive, optFromMax);
    }
}

runTest({m: +1, t: +1});
runTest({m: +1, t: -1});
runTest({m: -1, t: +1});
runTest({m: -1, t: -1});
})();
