// Confirms correct behavior for hinted aggregation execution. This includes tests for scenarios
// where agg execution differs from query. It also includes confirmation that hint works for find
// command against views, which is converted to a hinted aggregation on execution.
//
// @tags: [
//   does_not_support_stepdowns,
//   # Explain of a resolved view must be executed by mongos.
//   directly_against_shardsvrs_incompatible,
// ]
import {getAggPlanStages, getOptimizer, getPlanStages} from "jstests/libs/analyze_plan.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const isHintsToQuerySettingsSuite = TestData.isHintsToQuerySettingsSuite || false;

const testDB = db.getSiblingDB("agg_hint");
assert.commandWorked(testDB.dropDatabase());
const collName = jsTestName() + "_col"
const coll = testDB.getCollection(collName);
const viewName = jsTestName() + "_view"
const view = testDB.getCollection(viewName);

function confirmWinningPlanUsesExpectedIndex(
    explainResult, expectedKeyPattern, stageName, pipelineOptimizedAway) {
    const optimizer = getOptimizer(explainResult);

    if (!(optimizer in stageName) || stageName[optimizer] === "") {
        // TODO SERVER-77719: Ensure that the expected operator is defined for all optimizers. There
        // should be an exception here.
        return;
    }

    const planStages = pipelineOptimizedAway
        ? getPlanStages(explainResult, stageName[optimizer])
        : getAggPlanStages(explainResult, stageName[optimizer]);

    switch (optimizer) {
        case "classic":
            assert.neq(null, planStages, tojson(explainResult));
            planStages.forEach(planStage => {
                assert.eq(planStage.keyPattern, expectedKeyPattern, tojson(planStage));
            });
            break;
        case "CQF":
            // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
            // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
            break;
    }
}

// Runs explain on 'command', with the hint specified by 'hintKeyPattern' when not null.
// Confirms that the winning query plan uses the index specified by 'expectedKeyPattern'.
// If 'pipelineOptimizedAway' is set to true, then we expect the pipeline to be entirely
// optimized away from the plan and replaced with a query tier.
function confirmCommandUsesIndex({
    command = null,
    hintKeyPattern = null,
    expectedKeyPattern = null,
    stageName = {
        "classic": "IXSCAN",
        "CQF": "IndexScan"
    },
    pipelineOptimizedAway = false
} = {}) {
    if (hintKeyPattern) {
        command["hint"] = hintKeyPattern;
    }
    const res =
        assert.commandWorked(testDB.runCommand({explain: command, verbosity: "queryPlanner"}));
    confirmWinningPlanUsesExpectedIndex(res, expectedKeyPattern, stageName, pipelineOptimizedAway);
}

// Runs explain on an aggregation with a pipeline specified by 'aggPipeline' and a hint
// specified by 'hintKeyPattern' if not null. Confirms that the winning query plan uses the
// index specified by 'expectedKeyPattern'. If 'pipelineOptimizedAway' is set to true, then
// we expect the pipeline to be entirely optimized away from the plan and replaced with a
// query tier.
//
// This method exists because the explain command does not support the aggregation command.
function confirmAggUsesIndex({
    collName = null,
    aggPipeline = [],
    hintKeyPattern = null,
    expectedKeyPattern = null,
    stageName = {
        "classic": "IXSCAN",
        "CQF": "IndexScan"
    },
    pipelineOptimizedAway = false
} = {}) {
    let options = {};

    if (hintKeyPattern) {
        options = {hint: hintKeyPattern};
    }
    const res = assert.commandWorked(
        testDB.getCollection(collName).explain("executionStats").aggregate(aggPipeline, options));
    confirmWinningPlanUsesExpectedIndex(res, expectedKeyPattern, stageName, pipelineOptimizedAway);
}

// Specify hint as a string, representing index name.
assert.commandWorked(coll.createIndex({x: 1}));
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({x: i}));
}

confirmAggUsesIndex({
    collName: coll.getName(),
    aggPipeline: [{$match: {x: 3}}],
    hintKeyPattern: "x_1",
    expectedKeyPattern: {x: 1},
    pipelineOptimizedAway: true
});

//
// For each of the following tests we confirm:
// * That the expected index is chosen by the query planner when no hint is provided.
// * That the expected index is chosen when hinted.
// * That an index other than the one expected is chosen when hinted.
//

// Hint on poor index choice should force use of the hinted index over one more optimal.
assertDropCollection(testDB, collName);
assert.commandWorked(coll.createIndex({x: 1}));
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({x: i}));
}

confirmAggUsesIndex({
    collName: coll.getName(),
    aggPipeline: [{$match: {x: 3}}],
    expectedKeyPattern: {x: 1},
    pipelineOptimizedAway: true
});
confirmAggUsesIndex({
    collName: coll.getName(),
    aggPipeline: [{$match: {x: 3}}],
    hintKeyPattern: {x: 1},
    expectedKeyPattern: {x: 1},
    pipelineOptimizedAway: true
});

// Query settings do not force indexes and therefore '_id' index is not used when filtering on 'x'.
if (!isHintsToQuerySettingsSuite) {
    confirmAggUsesIndex({
        collName: coll.getName(),
        aggPipeline: [{$match: {x: 3}}],
        hintKeyPattern: {_id: 1},
        expectedKeyPattern: {_id: 1},
        pipelineOptimizedAway: true
    });
}

// With no hint specified, aggregation will always prefer an index that provides sort order over
// one that requires a blocking sort. A hinted aggregation should allow for choice of an index
// that provides blocking sort.
assertDropCollection(testDB, collName);
assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({y: 1}));
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({x: i, y: i}));
}

confirmAggUsesIndex({
    collName: coll.getName(),
    aggPipeline: [{$match: {x: {$gte: 0}}}, {$sort: {y: 1}}],
    expectedKeyPattern: {y: 1},
    pipelineOptimizedAway: true
});
confirmAggUsesIndex({
    collName: coll.getName(),
    aggPipeline: [{$match: {x: {$gte: 0}}}, {$sort: {y: 1}}],
    hintKeyPattern: {y: 1},
    expectedKeyPattern: {y: 1},
    pipelineOptimizedAway: true
});
confirmAggUsesIndex({
    collName: coll.getName(),
    aggPipeline: [{$match: {x: {$gte: 0}}}, {$sort: {y: 1}}],
    hintKeyPattern: {x: 1},
    expectedKeyPattern: {x: 1},
    pipelineOptimizedAway: true
});

// With no hint specified, aggregation will always prefer an index that provides a covered
// projection over one that does not. A hinted aggregation should allow for choice of an index
// that does not cover.
assertDropCollection(testDB, collName);
assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({x: 1, y: 1}));
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({x: i, y: i}));
}

confirmAggUsesIndex({
    collName: coll.getName(),
    aggPipeline: [{$match: {x: {$gte: 0}}}, {$project: {x: 1, y: 1, _id: 0}}],
    expectedKeyPattern: {x: 1, y: 1},
    pipelineOptimizedAway: true
});
confirmAggUsesIndex({
    collName: coll.getName(),
    aggPipeline: [{$match: {x: {$gte: 0}}}, {$project: {x: 1, y: 1, _id: 0}}],
    hintKeyPattern: {x: 1, y: 1},
    expectedKeyPattern: {x: 1, y: 1},
    pipelineOptimizedAway: true
});
confirmAggUsesIndex({
    collName: coll.getName(),
    aggPipeline: [{$match: {x: {$gte: 0}}}, {$project: {x: 1, y: 1, _id: 0}}],
    hintKeyPattern: {x: 1},
    expectedKeyPattern: {x: 1},
    pipelineOptimizedAway: true
});

// Confirm that a hinted agg can be executed against a view.
assertDropCollection(testDB, collName);
assertDropCollection(testDB, viewName);
assert.commandWorked(coll.createIndex({x: 1}));
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({x: i}));
}
assert.commandWorked(testDB.createView(viewName, collName, [{$match: {x: {$gte: 0}}}]));

confirmAggUsesIndex({
    collName: view.getName(),
    aggPipeline: [{$match: {x: 3}}],
    expectedKeyPattern: {x: 1},
    pipelineOptimizedAway: true
});
confirmAggUsesIndex({
    collName: view.getName(),
    aggPipeline: [{$match: {x: 3}}],
    hintKeyPattern: {x: 1},
    expectedKeyPattern: {x: 1},
    pipelineOptimizedAway: true
});

// Query settings do not force indexes and therefore '_id' index is not used when filtering on 'x'.
if (!isHintsToQuerySettingsSuite) {
    confirmAggUsesIndex({
        collName: view.getName(),
        aggPipeline: [{$match: {x: 3}}],
        hintKeyPattern: {_id: 1},
        expectedKeyPattern: {_id: 1},
        pipelineOptimizedAway: true
    });
}

// Confirm that a hinted find can be executed against a view.
assertDropCollection(testDB, collName);
assertDropCollection(testDB, viewName);
assert.commandWorked(coll.createIndex({x: 1}));
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({x: i}));
}
assert.commandWorked(testDB.createView(viewName, collName, []));

confirmCommandUsesIndex({
    command: {find: view.getName(), filter: {x: 3}},
    expectedKeyPattern: {x: 1},
    pipelineOptimizedAway: true
});
confirmCommandUsesIndex({
    command: {find: view.getName(), filter: {x: 3}},
    hintKeyPattern: {x: 1},
    expectedKeyPattern: {x: 1},
    pipelineOptimizedAway: true
});

// Query settings do not force indexes and therefore '_id' index is not used when filtering on 'x'.
if (!isHintsToQuerySettingsSuite) {
    confirmCommandUsesIndex({
        command: {find: view.getName(), filter: {x: 3}},
        hintKeyPattern: {_id: 1},
        expectedKeyPattern: {_id: 1},
        pipelineOptimizedAway: true
    });
}

// Confirm that a hinted count can be executed against a view.
assertDropCollection(testDB, collName);
assertDropCollection(testDB, viewName);
assert.commandWorked(coll.createIndex({x: 1}));
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({x: i}));
}
assert.commandWorked(testDB.createView(viewName, collName, []));

// TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
// optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
confirmCommandUsesIndex({
    command: {count: view.getName(), query: {x: 3}},
    expectedKeyPattern: {x: 1},
    stageName: {"classic": "COUNT_SCAN", "CQF": ""},
});
confirmCommandUsesIndex({
    command: {count: view.getName(), query: {x: 3}},
    hintKeyPattern: {x: 1},
    expectedKeyPattern: {x: 1},
    stageName: {"classic": "COUNT_SCAN", "CQF": ""},
});

// Query settings do not force indexes and therefore '_id' index is not used when filtering on 'x'.
if (!isHintsToQuerySettingsSuite) {
    confirmCommandUsesIndex({
        command: {count: view.getName(), query: {x: 3}},
        hintKeyPattern: {_id: 1},
        expectedKeyPattern: {_id: 1},
    });
}
