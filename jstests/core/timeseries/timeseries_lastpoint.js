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
 *   requires_getmore
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

function verifyTsResults(pipeline, index, precedingFilter) {
    // Prepare collections.
    const numHosts = 10;
    const numIterations = 20;
    const [tsColl, observerColl] =
        TimeseriesAggTests.prepareInputCollections(numHosts, numIterations);
    if (index) {
        tsColl.createIndex(index);
    }

    // Verify lastpoint optmization.
    const explain = tsColl.explain().aggregate(pipeline);
    if (index) {
        // The query can utilize DISTINCT_SCAN.
        assert.neq(getAggPlanStage(explain, "DISTINCT_SCAN"), null, explain);

        // Pipelines that use the DISTINCT_SCAN optimization should not also have a blocking sort.
        assert.eq(getAggPlanStage(explain, "SORT"), null, explain);
    } else {
        // $sort can be pushed into the cursor layer.
        assert.neq(getAggPlanStage(explain, "SORT"), null, explain);

        // At the bottom, there should be a COLLSCAN.
        const collScanStage = getAggPlanStage(explain, "COLLSCAN");
        assert.neq(collScanStage, null, explain);
        if (precedingFilter) {
            assert.eq(precedingFilter, collScanStage.filter, collScanStage);
        }
    }

    // Assert that the time-series aggregation results match that of the observer collection.
    const expectedResults = observerColl.aggregate(pipeline).toArray();
    const actualResults = tsColl.aggregate(pipeline).toArray();
    assert(resultsEq(actualResults, expectedResults),
           `Expected ${tojson(expectedResults)} but got ${tojson(actualResults)}`);
}

function verifyTsResultsWithAndWithoutIndex(pipeline, index, precedingFilter) {
    verifyTsResults(pipeline, undefined, precedingFilter);
    verifyTsResults(pipeline, index, precedingFilter);
}

verifyTsResultsWithAndWithoutIndex(
    [
        {$sort: {"tags.hostid": 1, time: -1}},
        {
            $group: {
                _id: "$tags.hostid",
                usage_user: {$first: "$usage_user"},
                usage_guest: {$first: "$usage_guest"},
                usage_idle: {$first: "$usage_idle"}
            }
        }
    ],
    {"tags.hostid": 1, time: -1});

verifyTsResultsWithAndWithoutIndex(
    [
        {$match: {"tags.hostid": "host_0"}},
        {$sort: {"tags.hostid": 1, time: -1}},
        {
            $group: {
                _id: "$tags.hostid",
                usage_user: {$first: "$usage_user"},
                usage_guest: {$first: "$usage_guest"},
                usage_idle: {$first: "$usage_idle"}
            }
        }
    ],
    {"tags.hostid": 1, time: -1},
    {"meta.hostid": {$eq: "host_0"}});
})();
