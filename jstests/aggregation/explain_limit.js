// Tests the behavior of explain() when used with the aggregation pipeline and limits.
// @tags: [do_not_wrap_aggregations_in_facets]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStages().

let coll = db.explain_limit;

const kCollSize = 105;
const kLimit = 10;

// Return whether or explain() was successful and contained the appropriate fields given the
// requested verbosity. Checks that the number of documents examined and returned are correct given
// the value of the limit.
function checkResults({results, verbosity}) {
    let cursorSubdocs = getAggPlanStages(results, "LIMIT");
    assert.gt(cursorSubdocs.length, 0);
    for (let stageResult of cursorSubdocs) {
        assert.eq(stageResult.limitAmount, NumberLong(kLimit), results);
        if (verbosity !== "queryPlanner") {
            assert.eq(stageResult.nReturned, NumberLong(kLimit), results);
        }
    }

    // Explain should report that we only have to examine as many documents as the limit.
    if (verbosity !== "queryPlanner") {
        if (results.hasOwnProperty("executionStats")) {
            assert.eq(results.executionStats.nReturned, kLimit, results);
            assert.eq(results.executionStats.totalDocsExamined, kLimit, results);
        } else {
            // This must be output for a sharded explain. Verify that each shard reports the
            // expected execution stats.
            assert(results.hasOwnProperty("shards"));
            for (let elem in results.shards) {
                const shardExecStats = results.shards[elem].executionStats;
                assert.eq(shardExecStats.nReturned, kLimit, results);
                assert.eq(shardExecStats.totalDocsExamined, kLimit, results);
            }
        }
    }
}

// explain() should respect limit.
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));

for (let i = 0; i < kCollSize; i++) {
    assert.commandWorked(coll.insert({a: 1}));
}

const pipeline = [{$match: {a: 1}}, {$limit: kLimit}];

let plannerLevel = coll.explain("queryPlanner").aggregate(pipeline);
checkResults({results: plannerLevel, verbosity: "queryPlanner"});

let execLevel = coll.explain("executionStats").aggregate(pipeline);
checkResults({results: execLevel, verbosity: "executionStats"});

let allPlansExecLevel = coll.explain("allPlansExecution").aggregate(pipeline);
checkResults({results: allPlansExecLevel, verbosity: "allPlansExecution"});

// Create a second index so that more than one plan is available.
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

plannerLevel = coll.explain("queryPlanner").aggregate(pipeline);
checkResults({results: plannerLevel, verbosity: "queryPlanner"});

execLevel = coll.explain("executionStats").aggregate(pipeline);
checkResults({results: execLevel, verbosity: "executionStats"});

allPlansExecLevel = coll.explain("allPlansExecution").aggregate(pipeline);
checkResults({results: allPlansExecLevel, verbosity: "allPlansExecution"});
})();
