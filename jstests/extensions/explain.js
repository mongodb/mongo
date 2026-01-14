/**
 * Tests that an extension can serialize itself correctly for explain.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db[jsTestName()];
coll.drop();

let docs = [];
for (let i = 0; i < 20; ++i) {
    docs.push({_id: i});
}
assert.commandWorked(coll.insertMany(docs));

function verifyPipelineExplainOutput(verbosity, explainOutput, expectedStages) {
    for (const stage of Object.keys(expectedStages)) {
        const stages = getAggPlanStages(explainOutput, stage);
        assert.gt(stages.length, 0, explainOutput);

        const stageOutput = stages[0];
        assert(stageOutput, explainOutput);
        assert.eq(stageOutput[stage], expectedStages[stage]);

        if (verbosity !== "queryPlanner") {
            if (stages.length === 1) {
                assert.eq(stageOutput.nReturned, docs.length);
            } else {
                for (const shardStage of stages) {
                    // In a sharded cluster, the results can be spread across shards, so relax the assertion.
                    assert.gt(shardStage.nReturned, 0, explainOutput);
                }
            }
            assert.eq(stageOutput.execMetricField, "execMetricValue");
            assert.gte(stageOutput.executionTimeMillisEstimate, 0);
        }
    }
}

function runTest(verbosity, pipeline, expectedStages) {
    const result = coll.explain(verbosity).aggregate(pipeline);
    verifyPipelineExplainOutput(verbosity, result, expectedStages);
}

// Test each verbosity for the $explain stage. $explain includes its input as well as a verbosity string in its output.
runTest("queryPlanner", [{$explain: {input: "hello"}}], {$explain: {input: "hello", verbosity: "queryPlanner"}});
runTest("executionStats", [{$explain: {input: "hello"}}], {$explain: {input: "hello", verbosity: "executionStats"}});
runTest("allPlansExecution", [{$explain: {input: "hello"}}], {
    $explain: {input: "hello", verbosity: "allPlansExecution"},
});

function runUnionWithTest(verbosity, pipeline, expectedStages) {
    const result = coll.explain(verbosity).aggregate({$unionWith: {coll: coll.getName(), pipeline}});

    for (const unionWithStage of getAggPlanStages(result, "$unionWith")) {
        const unionWithPipelineStages = unionWithStage["$unionWith"]["pipeline"];
        // A $unionWith subpipeline looks the same as a top-level pipeline in the explain output,
        // just nested differently. We can reuse our existing validation if we present it like a
        // top-level pipeline.
        verifyPipelineExplainOutput(verbosity, {stages: unionWithPipelineStages}, expectedStages);
    }
}

// TODO SERVER-117134 Enable once sharded $explain is fixed for extension stages in $unionWith.
if (!FixtureHelpers.isMongos(db)) {
    // Test the $explain stage in a $unionWith subpipeline.
    runUnionWithTest("queryPlanner", [{$explain: {input: "hello"}}], {
        $explain: {input: "hello", verbosity: "queryPlanner"},
    });
    runUnionWithTest("executionStats", [{$explain: {input: "hello"}}], {
        $explain: {input: "hello", verbosity: "executionStats"},
    });
}
