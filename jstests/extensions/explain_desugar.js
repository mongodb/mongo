/**
 * Tests that a desugar extension stage serializes itself correctly for explain.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagVectorSimilarityExpressions,
 *   requires_fcv_82,
 * ]
 */
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();
coll.insertMany([
    {_id: 1, embedding: [1, 0]},
    {_id: 2, embedding: [0.5, 0.5]},
]);

function explainNativeVectorSearch(spec, verbosity = "queryPlanner") {
    return coll.explain(verbosity).aggregate([{$nativeVectorSearch: spec}]);
}

function assertVectorSearchExplainContainsMetrics(expl, verbosity) {
    // Expected:
    //   1. $cursor
    //   2. $vectorSearchMetrics
    //   3. $setMetadata
    //   4. $sort (limit is folded into sort)
    const metricsStageName = "$vectorSearchMetrics";
    const metricsStages = getAggPlanStages(expl, metricsStageName);
    assert.eq(metricsStages[0].$vectorSearchMetrics, {"algorithm": "cosine"});
    if (verbosity !== "queryPlanner") {
        if (metricsStages.length === 1) {
            assert.eq(metricsStages[0].nReturned, 2);
        } else {
            for (const shardStage of metricsStages) {
                // In a sharded cluster the results can be spread across shards, so relax the assertion.
                assert.gte(shardStage.nReturned, 0);
            }
        }
        assert.gte(metricsStages[0].executionTimeMillisEstimate, 0);
        assert.lte(metricsStages[0].latestStart, Date.now());
    }

    const sortStageName = "$sort";
    const sortStages = getAggPlanStages(expl, sortStageName);
    assert.eq(sortStages[0].$sort.limit, 2);
}

function runTest(verbosity) {
    const simpleSpec = {
        path: "embedding",
        queryVector: [1, 0],
        limit: 2,
        metric: "cosine",
    };
    const expl = explainNativeVectorSearch(simpleSpec, verbosity);
    assertVectorSearchExplainContainsMetrics(expl, verbosity);
}

// Explain output contains post-desugar stages.
runTest("queryPlanner");

// Explain with executionStats gets the custom metrics from $vectorSearchMetrics.
runTest("executionStats");
