/**
 * Tests the behavior of the $_internalBoundedSort stage with a compound sort key.
 *
 * @tags: [
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # TODO (SERVER-88275) a moveCollection can cause the original collection to be dropped and
 *   # re-created with a different uuid, causing the aggregation to fail with QueryPlannedKilled
 *   # when the mongos is fetching data from the shard using getMore(). Remove the tag the issue
 *   # is solved
 *   assumes_balancer_off,
 * ]
 */
import {getTimeseriesCollForRawOps, kRawOperationSpec} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
const bucketMaxSpanSeconds = db.getCollectionInfos({name: coll.getName()})[0].options.timeseries.bucketMaxSpanSeconds;

assert.commandWorked(coll.createIndex({m: 1, t: 1}));
assert.commandWorked(coll.createIndex({m: 1, t: -1}));
assert.commandWorked(coll.createIndex({m: -1, t: 1}));
assert.commandWorked(coll.createIndex({m: -1, t: -1}));

// Insert some data.
{
    const numSeries = 5;
    const numBatchesPerSeries = 5;
    const batchSize = 120; // 2 hours, if events are 1 minute apart
    const start = new Date();
    const intervalMillis = 1000 * 60; // 1 minute

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
    const expectedBucketsPerSeries = Math.floor(
        (numBatchesPerSeries * batchSize * intervalMillis) / (bucketMaxSpanSeconds * 1000),
    );
    assert.gte(
        getTimeseriesCollForRawOps(coll)
            .aggregate([{$count: "n"}], kRawOperationSpec)
            .next().n,
        expectedBucketsPerSeries * numSeries,
        `Expected at least ${expectedBucketsPerSeries} buckets per series ` +
            `(${expectedBucketsPerSeries}*${numSeries} total)`,
    );

    TimeseriesTest.ensureDataIsDistributedIfSharded(coll, new Date(+start + (batchSize / 2) * intervalMillis));
}

const unpackStage = getAggPlanStages(coll.explain().aggregate(), "$_internalUnpackBucket")[0];

function inOrder(prev, doc, sortSpec) {
    const signum = (v) => {
        if (v > 0) return +1;
        if (v < 0) return -1;
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
    assert.eq(["m", "t"], Object.keys(sortSpec), "Expected a compound sort on {m: _, t: _}");
    assert.contains(sortSpec.m, [-1, +1]);
    assert.contains(sortSpec.t, [-1, +1]);

    if (result.length === 0) return;

    let prev = result[0];
    for (const doc of result.slice(1)) {
        assert(
            inOrder(prev, doc, sortSpec),
            "Found two docs not in " + tojson(sortSpec) + " order: " + tojson({prev, doc}),
        );

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

function runTest(sortSpec) {
    assert.eq(["m", "t"], Object.keys(sortSpec), "Expected a compound sort on {m: _, t: _}");
    assert.contains(sortSpec.m, [-1, +1]);
    assert.contains(sortSpec.t, [-1, +1]);

    // Test sorting the whole collection
    {
        const naiveQuery = [unpackStage, {$_internalInhibitOptimization: {}}, {$sort: sortSpec}];
        const reference = getTimeseriesCollForRawOps(coll).aggregate(naiveQuery, kRawOperationSpec).toArray();
        assertSorted(reference, sortSpec);

        // Check plan using control.min.t
        if (sortSpec.t > 0) {
            checkAgainstReference(reference, [{$sort: sortSpec}], sortSpec, sortSpec);
        } else {
            const optFromMinQuery = [{$sort: {m: sortSpec.m, t: sortSpec.t}}];
            const optFromMin = coll.aggregate(optFromMinQuery).toArray();
            assertSorted(optFromMin, sortSpec);
            assert.eq(reference, optFromMin);
        }

        // Check plan using control.max.t
        if (sortSpec.t > 0) {
            const optFromMaxQuery = [{$sort: {m: sortSpec.m, t: sortSpec.t}}];
            const optFromMax = coll.aggregate(optFromMaxQuery, {hint: {m: -sortSpec.m, t: -sortSpec.t}}).toArray();
            assertSorted(optFromMax, sortSpec);
            assert.eq(reference, optFromMax);
        } else {
            checkAgainstReference(reference, [{$sort: sortSpec}], sortSpec, sortSpec);
        }
    }

    // Test $sort + $limit.
    {
        const naiveQuery = [unpackStage, {$_internalInhibitOptimization: {}}, {$sort: sortSpec}, {$limit: 100}];
        const naive = getTimeseriesCollForRawOps(coll).aggregate(naiveQuery, kRawOperationSpec).toArray();
        assertSorted(naive, sortSpec);
        assert.eq(100, naive.length);

        const optFromMinQuery = [
            {$sort: {meta: sortSpec.m, "control.min.t": sortSpec.t}},
            unpackStage,
            {
                $_internalBoundedSort: {
                    sortKey: sortSpec,
                    bound: sortSpec.t > 0 ? {base: "min"} : {base: "min", offsetSeconds: bucketMaxSpanSeconds},
                    limit: 100,
                },
            },
        ];
        const optFromMin = getTimeseriesCollForRawOps(coll).aggregate(optFromMinQuery, kRawOperationSpec).toArray();
        assertSorted(optFromMin, sortSpec);
        assert.eq(100, optFromMin.length);
        assert.eq(naive, optFromMin);

        const optFromMaxQuery = [
            {$sort: {meta: sortSpec.m, "control.max.t": sortSpec.t}},
            unpackStage,
            {
                $_internalBoundedSort: {
                    sortKey: sortSpec,
                    bound: sortSpec.t > 0 ? {base: "max", offsetSeconds: -bucketMaxSpanSeconds} : {base: "max"},
                    limit: 100,
                },
            },
        ];
        const optFromMax = getTimeseriesCollForRawOps(coll).aggregate(optFromMaxQuery, kRawOperationSpec).toArray();
        assertSorted(optFromMax, sortSpec);
        assert.eq(100, optFromMax.length);
        assert.eq(naive, optFromMax);
    }
}

runTest({m: +1, t: +1});
runTest({m: +1, t: -1});
runTest({m: -1, t: +1});
runTest({m: -1, t: -1});
