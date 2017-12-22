// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop, does_not_support_stepdowns]

// Confirms correct behavior for hinted aggregation execution. This includes tests for scenarios
// where agg execution differs from query. It also includes confirmation that hint works for find
// command against views, which is converted to a hinted aggregation on execution.

(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");  // For getAggPlanStage.

    const testDB = db.getSiblingDB("agg_hint");
    assert.commandWorked(testDB.dropDatabase());
    const coll = testDB.getCollection("test");
    const view = testDB.getCollection("view");
    const NO_HINT = null;

    function confirmWinningPlanUsesExpectedIndex(explainResult, expectedKeyPattern, stageName) {
        const planStage = getAggPlanStage(explainResult, stageName);
        assert.neq(null, planStage);

        assert.eq(planStage.keyPattern, expectedKeyPattern, tojson(planStage));
    }

    // Runs explain on 'command', with the hint specified by 'hintKeyPattern' when not null.
    // Confirms that the winning query plan uses the index specified by 'expectedKeyPattern'.
    function confirmCommandUsesIndex(
        command, hintKeyPattern, expectedKeyPattern, stageName = "IXSCAN") {
        if (hintKeyPattern) {
            command["hint"] = hintKeyPattern;
        }
        const res =
            assert.commandWorked(testDB.runCommand({explain: command, verbosity: "queryPlanner"}));
        confirmWinningPlanUsesExpectedIndex(res, expectedKeyPattern, stageName);
    }

    // Runs explain on an aggregation with a pipeline specified by 'aggPipeline' and a hint
    // specified by 'hintKeyPattern' if not null. Confirms that the winning query plan uses the
    // index specified by 'expectedKeyPattern'.
    //
    // This method exists because the explain command does not support the aggregation command.
    function confirmAggUsesIndex(
        collName, aggPipeline, hintKeyPattern, expectedKeyPattern, stageName = "IXSCAN") {
        let options = {};
        if (hintKeyPattern) {
            options = {hint: hintKeyPattern};
        }
        const res = assert.commandWorked(
            testDB.getCollection(collName).explain().aggregate(aggPipeline, options));
        confirmWinningPlanUsesExpectedIndex(res, expectedKeyPattern, stageName);
    }

    // Specify hint as a string, representing index name.
    assert.commandWorked(coll.createIndex({x: 1}));
    for (let i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({x: i}));
    }

    confirmAggUsesIndex("test", [{$match: {x: 3}}], "x_1", {x: 1});

    //
    // For each of the following tests we confirm:
    // * That the expected index is chosen by the query planner when no hint is provided.
    // * That the expected index is chosen when hinted.
    // * That an index other than the one expected is chosen when hinted.
    //

    // Hint on poor index choice should force use of the hinted index over one more optimal.
    coll.drop();
    assert.commandWorked(coll.createIndex({x: 1}));
    for (let i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({x: i}));
    }

    confirmAggUsesIndex("test", [{$match: {x: 3}}], NO_HINT, {x: 1});
    confirmAggUsesIndex("test", [{$match: {x: 3}}], {x: 1}, {x: 1});
    confirmAggUsesIndex("test", [{$match: {x: 3}}], {_id: 1}, {_id: 1});

    // With no hint specified, aggregation will always prefer an index that provides sort order over
    // one that requires a blocking sort. A hinted aggregation should allow for choice of an index
    // that provides blocking sort.
    coll.drop();
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({y: 1}));
    for (let i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({x: i, y: i}));
    }

    confirmAggUsesIndex("test", [{$match: {x: {$gte: 0}}}, {$sort: {y: 1}}], NO_HINT, {y: 1});
    confirmAggUsesIndex("test", [{$match: {x: {$gte: 0}}}, {$sort: {y: 1}}], {y: 1}, {y: 1});
    confirmAggUsesIndex("test", [{$match: {x: {$gte: 0}}}, {$sort: {y: 1}}], {x: 1}, {x: 1});

    // With no hint specified, aggregation will always prefer an index that provides a covered
    // projection over one that does not. A hinted aggregation should allow for choice of an index
    // that does not cover.
    coll.drop();
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({x: 1, y: 1}));
    for (let i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({x: i, y: i}));
    }

    confirmAggUsesIndex("test",
                        [{$match: {x: {$gte: 0}}}, {$project: {x: 1, y: 1, _id: 0}}],
                        NO_HINT,
                        {x: 1, y: 1});
    confirmAggUsesIndex("test",
                        [{$match: {x: {$gte: 0}}}, {$project: {x: 1, y: 1, _id: 0}}],
                        {x: 1, y: 1},
                        {x: 1, y: 1});
    confirmAggUsesIndex(
        "test", [{$match: {x: {$gte: 0}}}, {$project: {x: 1, y: 1, _id: 0}}], {x: 1}, {x: 1});

    // Confirm that a hinted agg can be executed against a view.
    coll.drop();
    view.drop();
    assert.commandWorked(coll.createIndex({x: 1}));
    for (let i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({x: i}));
    }
    assert.commandWorked(testDB.createView("view", "test", []));

    confirmAggUsesIndex("view", [{$match: {x: 3}}], NO_HINT, {x: 1});
    confirmAggUsesIndex("view", [{$match: {x: 3}}], {x: 1}, {x: 1});
    confirmAggUsesIndex("view", [{$match: {x: 3}}], {_id: 1}, {_id: 1});

    // Confirm that a hinted find can be executed against a view.
    coll.drop();
    view.drop();
    assert.commandWorked(coll.createIndex({x: 1}));
    for (let i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({x: i}));
    }
    assert.commandWorked(testDB.createView("view", "test", []));

    confirmCommandUsesIndex({find: "view", filter: {x: 3}}, NO_HINT, {x: 1});
    confirmCommandUsesIndex({find: "view", filter: {x: 3}}, {x: 1}, {x: 1});
    confirmCommandUsesIndex({find: "view", filter: {x: 3}}, {_id: 1}, {_id: 1});

    // Confirm that a hinted count can be executed against a view.
    coll.drop();
    view.drop();
    assert.commandWorked(coll.createIndex({x: 1}));
    for (let i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({x: i}));
    }
    assert.commandWorked(testDB.createView("view", "test", []));

    confirmCommandUsesIndex({count: "view", query: {x: 3}}, NO_HINT, {x: 1}, "COUNT_SCAN");
    confirmCommandUsesIndex({count: "view", query: {x: 3}}, {x: 1}, {x: 1}, "COUNT_SCAN");
    confirmCommandUsesIndex({count: "view", query: {x: 3}}, {_id: 1}, {_id: 1});
})();