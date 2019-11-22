/**
 * Tests that running mapReduce with explain behaves as expected.
 *
 * TODO SERVER-42511: Remove 'does_not_support_stepdowns' tag once query knob is removed.
 * @tags: [does_not_support_stepdowns, requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getPlanStages.

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

const mr = {
    mapReduce: coll.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: "inline"
};
try {
    // Succeeds for all modes when using agg map reduce.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryUseAggMapReduce: true}));
    for (let verbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
        const results = coll.explain(verbosity).mapReduce(mr);

        // Check server info
        assert(results.hasOwnProperty('serverInfo'), results);
        assert.hasFields(results.serverInfo, ['host', 'port', 'version', 'gitVersion']);

        const stages = getAggPlanStages(results, "$cursor");
        assert(stages !== null);

        // Verify that explain's output contains the fields that we expect.
        // We loop through in the case that explain is run against a sharded cluster.
        for (var i = 0; i < stages.length; i++) {
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
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryUseAggMapReduce: false}));
}
}());
