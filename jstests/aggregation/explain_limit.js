// Tests the behavior of explain() when used with the aggregation pipeline and limits.
//
// This test makes assumptions about how the explain output will be formatted, so cannot be
// transformed to be put inside a $facet stage, or when pipeline optimization is disabled.
// @tags: [
//   do_not_wrap_aggregations_in_facets,
//   requires_pipeline_optimization,
// ]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStages().

let coll = db.explain_limit;

const kCollSize = 105;
const kLimit = 10;

// Note that the "getParameter" command is expected to fail in versions of mongod that do not yet
// include the slot-based execution engine. When that happens, however, 'isSBEEnabled' still
// correctly evaluates to false.
const isSBEEnabled = (() => {
    const getParam = db.adminCommand({getParameter: 1, featureFlagSBE: 1});
    return getParam.hasOwnProperty("featureFlagSBE") && getParam.featureFlagSBE.value;
})();

// Return whether or explain() was successful and contained the appropriate fields given the
// requested verbosity. Checks that the number of documents examined and returned are correct given
// the value of the limit.
function checkResults({results, verbosity}) {
    const [cursorSubdocs, limitAmount] = (() => {
        if (verbosity != "queryPlanner" && isSBEEnabled) {
            // We cannot use the "executionStats" section for SBE plans without some pre-processing,
            // since it has different explain format. To find execution stats for the LIMIT stages
            // from the "queryPlanner" section (there could be multiple of such stages if we're in a
            // sharded environment, one for each shard) we first extract their 'planNodeIds' into a
            // set. Then we filter out all "limit" stages from the "executionStats" section by
            // keeping only correlated plan stages, and return the final array. Since the name of
            // the field holding the limit amount also differs in SBE, we return a proper name as
            // well.
            const useQueryPlannerSection = true;
            const queryPlannerStages = getAggPlanStages(results, "LIMIT", useQueryPlannerSection);
            const execStatsStages = getAggPlanStages(results, "limit");
            assert.gt(queryPlannerStages.length, 0, results);
            assert.gt(execStatsStages.length, 0, results);
            const planNodeIds = new Set(queryPlannerStages.map(stage => stage.planNodeId));
            return [execStatsStages.filter(stage => planNodeIds.has(stage.planNodeId)), "limit"];
        }
        return [getAggPlanStages(results, "LIMIT"), "limitAmount"];
    })();
    assert.gt(cursorSubdocs.length, 0, results);
    for (let stageResult of cursorSubdocs) {
        assert.eq(stageResult[limitAmount], NumberLong(kLimit), cursorSubdocs);
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
