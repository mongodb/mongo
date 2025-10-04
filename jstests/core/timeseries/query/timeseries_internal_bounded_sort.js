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

// Create a descending time index to allow sorting by control.max.t
assert.commandWorked(coll.createIndex({t: -1}));

// Insert some data.
{
    const numBatches = 10;
    const batchSize = 1000;
    const intervalMillis = 100000; // 100 seconds
    const batchOffset = Math.floor(intervalMillis / (numBatches + 1));
    const start = new Date();
    for (let i = 0; i < numBatches; ++i) {
        const batch = Array.from({length: batchSize}, (_, j) => ({
            t: new Date(+start + i * batchOffset + j * intervalMillis),
        }));
        assert.commandWorked(coll.insert(batch));
        print(`Inserted ${i + 1} of ${numBatches} batches`);
    }
    assert.gt(
        getTimeseriesCollForRawOps(coll)
            .aggregate([{$count: "n"}], kRawOperationSpec)
            .next().n,
        1,
        "Expected more than one bucket",
    );

    TimeseriesTest.ensureDataIsDistributedIfSharded(coll, new Date(+start + (batchSize / 2) * intervalMillis));
}

const unpackStage = getAggPlanStages(coll.explain().aggregate(), "$_internalUnpackBucket")[0];

function assertSorted(result, ascending) {
    let prev = ascending ? {t: -Infinity} : {t: Infinity};
    for (const doc of result) {
        if (ascending) {
            assert.lte(+prev.t, +doc.t, "Found two docs not in ascending time order: " + tojson({prev, doc}));
        } else {
            assert.gte(+prev.t, +doc.t, "Found two docs not in descending time order: " + tojson({prev, doc}));
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
        const reference = getTimeseriesCollForRawOps(coll)
            .aggregate(
                [unpackStage, {$_internalInhibitOptimization: {}}, {$sort: {t: ascending ? 1 : -1}}],
                kRawOperationSpec,
            )
            .toArray();
        assertSorted(reference, ascending);

        // Check plan using control.min.t
        checkAgainstReference(
            reference,
            [{$sort: {t: ascending ? 1 : -1}}],
            {"$natural": ascending ? 1 : -1},
            ascending,
        );

        // Check plan using control.max.t
        if (ascending) {
            const opt = coll.aggregate([{$sort: {t: 1}}], {hint: {t: -1}}).toArray();
            assertSorted(opt, ascending);
            assert.eq(reference, opt);
        } else {
            checkAgainstReference(reference, [{$sort: {t: ascending ? 1 : -1}}], {"t": -1}, ascending);
        }
    }

    // Test $sort + $limit.
    {
        const naive = getTimeseriesCollForRawOps(coll)
            .aggregate(
                [unpackStage, {$_internalInhibitOptimization: {}}, {$sort: {t: ascending ? 1 : -1}}, {$limit: 100}],
                kRawOperationSpec,
            )
            .toArray();
        assertSorted(naive, ascending);
        assert.eq(100, naive.length);

        const optFromMin = getTimeseriesCollForRawOps(coll)
            .aggregate(
                [
                    {$sort: {"control.min.t": ascending ? 1 : -1}},
                    unpackStage,
                    {
                        $_internalBoundedSort: {
                            sortKey: {t: ascending ? 1 : -1},
                            bound: ascending ? {base: "min"} : {base: "min", offsetSeconds: bucketMaxSpanSeconds},
                            limit: 100,
                        },
                    },
                ],
                kRawOperationSpec,
            )
            .toArray();
        assertSorted(optFromMin, ascending);
        assert.eq(100, optFromMin.length);
        assert.eq(naive, optFromMin);

        const optFromMax = getTimeseriesCollForRawOps(coll)
            .aggregate(
                [
                    {$sort: {"control.max.t": ascending ? 1 : -1}},
                    unpackStage,
                    {
                        $_internalBoundedSort: {
                            sortKey: {t: ascending ? 1 : -1},
                            bound: ascending ? {base: "max", offsetSeconds: -bucketMaxSpanSeconds} : {base: "max"},
                            limit: 100,
                        },
                    },
                ],
                kRawOperationSpec,
            )
            .toArray();
        assertSorted(optFromMax, ascending);
        assert.eq(100, optFromMax.length);
        assert.eq(naive, optFromMax);
    }
}

runTest(true); // ascending
runTest(false); // descending
