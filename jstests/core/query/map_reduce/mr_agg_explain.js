/**
 * Tests that running mapReduce with explain behaves as expected.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: mapReduce.
 *   not_allowed_with_signed_security_token,
 *   # mapReduce does not support afterClusterTime.
 *   does_not_support_causal_consistency,
 *   requires_scripting,
 * ]
 */
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

const coll = db.mr_explain;
coll.drop();
assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.insert({x: 1}));

function mapFunc() {
    emit(this.x, 1);
}

function reduceFunc(key, values) {
    return Array.sum(values);
}

function runTest(optionsObjOrOutString) {
    // Succeeds for all modes when using agg map reduce.
    for (let verbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
        const results = coll.explain(verbosity).mapReduce(mapFunc, reduceFunc, optionsObjOrOutString);

        // Check server info
        assert(results.hasOwnProperty("serverInfo"), results);
        assert.hasFields(results.serverInfo, ["host", "port", "version", "gitVersion"]);

        const stages = getAggPlanStages(results, "$cursor");
        assert(stages !== null);

        // Verify that explain's output contains the fields that we expect.
        // We loop through in the case that explain is run against a sharded cluster.
        for (let i = 0; i < stages.length; i++) {
            const stage = stages[i]["$cursor"];
            if (verbosity != "allPlansExecution") {
                assert(stage.hasOwnProperty(verbosity));
            } else {
                assert(stage.hasOwnProperty("executionStats"));
                const execStats = stage["executionStats"];
                assert(execStats.hasOwnProperty(verbosity));
            }
        }
    }
}

// Test mapReduce explain with output to a collection.
runTest("out_collection");

// Test mapReduce explain with inline output.
runTest({out: {inline: 1}});

// Explain on mapReduce fails when the 3rd 'optionsOrOutString' argument is missing.
assert.throws(() => coll.explain().mapReduce(mapFunc, reduceFunc));
