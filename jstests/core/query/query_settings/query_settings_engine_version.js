// Tests query settings impact on the queries when 'queryEngineVersion' is set.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   does_not_support_stepdowns,
//   simulate_atlas_proxy_incompatible,
//   cqf_incompatible,
// ]
//

import {getPlanStages, getQueryPlanner, getWinningPlan} from "jstests/libs/analyze_plan.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

// Create the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());

// Create the index, such that we can ensure that index hints can be combined with query settings,
// when query settings specify only query engine version.
const indexKeyPattern = {
    a: 1,
    b: 1
};
assert.commandWorked(coll.createIndexes([{a: 1}, indexKeyPattern, {a: 1, b: 1, c: 1}]));

const sbeEligibleQuery = {
    find: coll.getName(),
    $db: db.getName(),
    filter: {a: {$lt: 2}},
    hint: indexKeyPattern,
};
const nonSbeEligibleQuery = {
    find: coll.getName(),
    $db: db.getName(),
    sort: {"a.0": 1},
    hint: indexKeyPattern,
};

function assertHintedIndexWasUsed(queryPlanner) {
    const winningPlan = getWinningPlan(queryPlanner);
    const ixscanStage = getPlanStages(winningPlan, "IXSCAN")[0];
    assert.eq(indexKeyPattern, ixscanStage.keyPattern, queryPlanner.winningPlan);
}

// Ensure that classic engine is used if 'queryEngineVersion' v1 is set, regardless of the query
// SBE eligibility.
{
    // Ensure that query settings cluster parameter is empty.
    qsutils.assertQueryShapeConfiguration([]);

    // Setting to use classic engine.
    const settings = {queryEngineVersion: "v1"};

    // Set query settings to queries to use the classic engine.
    assert.commandWorked(db.adminCommand({setQuerySettings: sbeEligibleQuery, settings: settings}));
    qsutils.assertQueryShapeConfiguration([
        qsutils.makeQueryShapeConfiguration(settings, sbeEligibleQuery),
    ]);
    assert.commandWorked(
        db.adminCommand({setQuerySettings: nonSbeEligibleQuery, settings: settings}));
    qsutils.assertQueryShapeConfiguration([
        qsutils.makeQueryShapeConfiguration(settings, sbeEligibleQuery),
        qsutils.makeQueryShapeConfiguration(settings, nonSbeEligibleQuery),
    ]);

    for (let query of [sbeEligibleQuery, nonSbeEligibleQuery]) {
        const {$db: _, ...queryWithoutDollarDb} = query;
        const explain = assert.commandWorked(db.runCommand({explain: queryWithoutDollarDb}));
        const queryPlanner = getQueryPlanner(explain);
        assert(!queryPlanner.winningPlan.hasOwnProperty("slotBasedPlan"), queryPlanner);
        assertHintedIndexWasUsed(queryPlanner);
    }

    qsutils.removeAllQuerySettings();
}

// Ensure that SBE engine is used if 'queryEngineVersion' v2 is set and query is eligible for
// SBE.
{
    // Ensure that query settings cluster parameter is empty.
    qsutils.assertQueryShapeConfiguration([]);

    // Setting to use the SBE engine, if possible.
    const settings = {queryEngineVersion: "v2"};

    // Set query settings to queries to use the SBE engine.
    assert.commandWorked(db.adminCommand({setQuerySettings: sbeEligibleQuery, settings: settings}));
    qsutils.assertQueryShapeConfiguration([
        qsutils.makeQueryShapeConfiguration(settings, sbeEligibleQuery),
    ]);
    assert.commandWorked(
        db.adminCommand({setQuerySettings: nonSbeEligibleQuery, settings: settings}));
    qsutils.assertQueryShapeConfiguration([
        qsutils.makeQueryShapeConfiguration(settings, sbeEligibleQuery),
        qsutils.makeQueryShapeConfiguration(settings, nonSbeEligibleQuery),
    ]);

    {
        const {$db: _, ...queryWithoutDollarDb} = nonSbeEligibleQuery;
        const explain = assert.commandWorked(db.runCommand({explain: queryWithoutDollarDb}));
        const queryPlanner = getQueryPlanner(explain);
        assert(!queryPlanner.winningPlan.hasOwnProperty("slotBasedPlan"), queryPlanner);
        assertHintedIndexWasUsed(queryPlanner);
    }

    {
        const {$db: _, ...queryWithoutDollarDb} = sbeEligibleQuery;
        const explain = assert.commandWorked(db.runCommand({explain: queryWithoutDollarDb}));
        const queryPlanner = getQueryPlanner(explain);
        assert(queryPlanner.winningPlan.hasOwnProperty("slotBasedPlan"), queryPlanner);
        assertHintedIndexWasUsed(queryPlanner);
    }

    qsutils.removeAllQuerySettings();
}
