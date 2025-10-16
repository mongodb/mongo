/**
 * Tests that an extension can serialize itself correctly for explain.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();

let docs = [];
for (let i = 0; i < 20; ++i) {
    docs.push({_id: i});
}
assert.commandWorked(coll.insertMany(docs));

function runTest(verbosity, pipeline, expectedStages) {
    const result = coll.explain(verbosity).aggregate(pipeline);

    for (const stage of Object.keys(expectedStages)) {
        const stages = getAggPlanStages(result, stage);
        assert.gt(stages.length, 0, result);

        const stageOutput = stages[0];
        assert(stageOutput, result);
        assert.eq(stageOutput[stage], expectedStages[stage]);

        if (verbosity !== "queryPlanner") {
            if (stages.length === 1) {
                assert.eq(stageOutput.nReturned, docs.length);
            } else {
                for (const shardStage of stages) {
                    // In a sharded cluster, the results can be spread across shards, so relax the assertion.
                    assert.gt(shardStage.nReturned, 0, result);
                }
            }

            // TODO SERVER-112395 Validate stage-specific execution stats.
        }
    }
}

// Test each verbosity for the $explain stage. $explain includes its input as well as a verbosity string in its output.
runTest("queryPlanner", [{$explain: {input: "hello"}}], {$explain: {input: "hello", verbosity: "queryPlanner"}});
runTest("executionStats", [{$explain: {input: "hello"}}], {$explain: {input: "hello", verbosity: "executionStats"}});
runTest("allPlansExecution", [{$explain: {input: "hello"}}], {
    $explain: {input: "hello", verbosity: "allPlansExecution"},
});

// TODO SERVER-112519 Add tests covering explain on a desugar stage.
