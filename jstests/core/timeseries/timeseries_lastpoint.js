/**
 * Tests the optimization of "lastpoint"-type queries on time-series collections.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_timeseries,
 *   requires_pipeline_optimization,
 *   requires_fcv_53,
 *   # TODO (SERVER-63590): Investigate presence of getmore tag in timeseries jstests.
 *   requires_getmore,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");
load("jstests/core/timeseries/libs/timeseries_agg_helpers.js");
load('jstests/libs/analyze_plan.js');

const testDB = TimeseriesAggTests.getTestDb();
assert.commandWorked(testDB.dropDatabase());

// Do not run the rest of the tests if the lastpoint optimization is disabled.
const getLastpointParam = db.adminCommand({getParameter: 1, featureFlagLastPointQuery: 1});
const isLastpointEnabled = getLastpointParam.hasOwnProperty("featureFlagLastPointQuery") &&
    getLastpointParam.featureFlagLastPointQuery.value;
if (!isLastpointEnabled) {
    return;
}

// Timeseries test parameters.
const numHosts = 10;
const numIterations = 20;

function verifyTsResults({pipeline, precedingFilter, expectStage, prepareTest}) {
    // Prepare collections. Note: we test without idle measurements (all meta subfields are
    // non-null). If we allow the insertion of idle measurements, we will obtain multiple lastpoints
    // per bucket, and may have different results on the observer and timeseries collections.
    const [tsColl, observerColl] = TimeseriesAggTests.prepareInputCollections(
        numHosts, numIterations, false /* includeIdleMeasurements */);

    // Additional preparation before running the test.
    if (prepareTest) {
        prepareTest(tsColl, observerColl);
    }

    // Verify lastpoint optmization.
    const explain = tsColl.explain().aggregate(pipeline);
    expectStage({explain, precedingFilter});

    // Assert that the time-series aggregation results match that of the observer collection.
    const expected = observerColl.aggregate(pipeline).toArray();
    const actual = tsColl.aggregate(pipeline).toArray();
    assertArrayEq({actual, expected});

    // Drop collections.
    tsColl.drop();
    observerColl.drop();
}

function verifyTsResultsWithAndWithoutIndex(
    {pipeline, index, bucketsIndex, precedingFilter, expectStage, prePrepareTest}) {
    verifyTsResults(
        {pipeline, precedingFilter, expectStage: expectCollScan, prepareTest: prePrepareTest});
    verifyTsResults({
        pipeline,
        precedingFilter,
        expectStage,
        prepareTest: (testColl, observerColl) => {
            // Optionally do extra test preparation.
            if (prePrepareTest) {
                prePrepareTest(testColl, observerColl);
            }

            // Create index on the timeseries collection.
            testColl.createIndex(index);

            // Create an additional secondary index directly on the buckets collection so that we
            // can test the DISTINCT_SCAN optimization when time is sorted in ascending order.
            if (bucketsIndex) {
                const bucketsColl = testDB["system.buckets.in"];
                bucketsColl.createIndex(bucketsIndex);
            }
        }
    });
}

function expectDistinctScan({explain}) {
    // The query can utilize DISTINCT_SCAN.
    assert.neq(getAggPlanStage(explain, "DISTINCT_SCAN"), null, explain);

    // Pipelines that use the DISTINCT_SCAN optimization should not also have a blocking sort.
    assert.eq(getAggPlanStage(explain, "SORT"), null, explain);
}

function expectCollScan({explain, precedingFilter}) {
    // $sort can be pushed into the cursor layer.
    assert.neq(getAggPlanStage(explain, "SORT"), null, explain);

    // At the bottom, there should be a COLLSCAN.
    const collScanStage = getAggPlanStage(explain, "COLLSCAN");
    assert.neq(collScanStage, null, explain);
    if (precedingFilter) {
        assert.eq(precedingFilter, collScanStage.filter, collScanStage);
    }
}

function expectIxscan({explain}) {
    // $sort can be pushed into the cursor layer.
    assert.neq(getAggPlanStage(explain, "SORT"), null, explain);

    // At the bottom, there should be a IXSCAN.
    assert.neq(getAggPlanStage(explain, "IXSCAN"), null, explain);
}

function getGroupStage(accumulator) {
    return {
        $group: {
            _id: "$tags.hostid",
            usage_user: {[accumulator]: "$usage_user"},
            usage_guest: {[accumulator]: "$usage_guest"},
            usage_idle: {[accumulator]: "$usage_idle"}
        }
    };
}

/**
    Test cases:
     1. Lastpoint queries on indexes with descending time and $first (DISTINCT_SCAN).
     2. Lastpoint queries on indexes with ascending time and $last (no DISTINCT_SCAN).
     3. Lastpoint queries on indexes with ascending time and $last and an additional secondary
    index so that we can use the DISTINCT_SCAN optimization.
*/
const testCases = [
    {time: -1, useBucketsIndex: false},
    {time: 1, useBucketsIndex: false},
    {time: 1, useBucketsIndex: true}
];

for (const {time, useBucketsIndex} of testCases) {
    const isTimeDescending = time < 0;
    const canUseDistinct = isTimeDescending || useBucketsIndex;
    const groupStage = isTimeDescending ? getGroupStage("$first") : getGroupStage("$last");

    // Test both directions of the metaField sort for each direction of time.
    for (const metaDir of [1, -1]) {
        const index = {"tags.hostid": metaDir, time};
        const bucketsIndex = useBucketsIndex
            ? {"meta.hostid": metaDir, "control.max.time": 1, "control.min.time": 1}
            : undefined;
        const canSortOnTimeUseDistinct = (metaDir > 0) && (isTimeDescending || useBucketsIndex);

        verifyTsResultsWithAndWithoutIndex({
            pipeline: [{$sort: {time}}, groupStage],
            index,
            bucketsIndex,
            expectStage: (canSortOnTimeUseDistinct ? expectDistinctScan : expectCollScan)
        });

        // Test pipeline without a preceding $match stage.
        verifyTsResultsWithAndWithoutIndex({
            pipeline: [{$sort: index}, groupStage],
            index,
            bucketsIndex,
            expectStage: (canUseDistinct ? expectDistinctScan : expectCollScan)
        });

        // Test pipeline without a preceding $match stage which has an extra idle measurement. This
        // verifies that the query rewrite correctly returns missing fields.
        verifyTsResultsWithAndWithoutIndex({
            pipeline: [{$sort: index}, groupStage],
            index,
            bucketsIndex,
            expectStage: (canUseDistinct ? expectDistinctScan : expectCollScan),
            prePrepareTest: (testColl, observerColl) => {
                const currTime = new Date();
                for (const host of TimeseriesTest.generateHosts(numHosts)) {
                    const idleMeasurement = {
                        tags: host.tags,
                        time: new Date(currTime + numIterations),  // Ensure this is the lastpoint.
                        idle_user: 100 - TimeseriesTest.getRandomUsage()
                    };
                    assert.commandWorked(testColl.insert(idleMeasurement));
                    assert.commandWorked(observerColl.insert(idleMeasurement));
                }
            }
        });

        // Test pipeline with a preceding $match stage.
        function testWithMatch(matchStage, precedingFilter) {
            verifyTsResultsWithAndWithoutIndex({
                pipeline: [matchStage, {$sort: index}, groupStage],
                index,
                bucketsIndex,
                expectStage: canUseDistinct ? expectDistinctScan : expectIxscan,
                precedingFilter
            });
        }

        // Test pipeline with an equality $match stage.
        testWithMatch({$match: {"tags.hostid": 0}}, {"meta.hostid": {$eq: 0}});

        // Test pipeline with an inequality $match stage.
        testWithMatch({$match: {"tags.hostid": {$ne: 0}}}, {"meta.hostid": {$not: {$eq: 0}}});

        // Test pipeline with a $match stage that uses a $gt query.
        testWithMatch({$match: {"tags.hostid": {$gt: 5}}}, {"meta.hostid": {$gt: 5}});

        // Test pipeline with a $match stage that uses a $lt query.
        testWithMatch({$match: {"tags.hostid": {$lt: 5}}}, {"meta.hostid": {$lt: 5}});
    }
}
})();
