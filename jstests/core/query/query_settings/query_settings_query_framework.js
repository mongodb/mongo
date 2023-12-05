// Tests query settings impact on the queries when 'queryFramework' is set.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   simulate_atlas_proxy_incompatible,
//   cqf_incompatible,
// ]
//

import {getEngine, getPlanStages, getWinningPlanFromExplain} from "jstests/libs/analyze_plan.js";
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

function assertHintedIndexWasUsed(explain) {
    const winningPlan = getWinningPlanFromExplain(explain);
    const ixscanStage = getPlanStages(winningPlan, "IXSCAN")[0];
    assert.eq(indexKeyPattern, ixscanStage.keyPattern, winningPlan);
}

function testQueryFramework({query, settings, expectedEngine}) {
    // Ensure that query settings cluster parameter is empty.
    qsutils.assertQueryShapeConfiguration([]);

    // Apply the provided settings for the query.
    assert.commandWorked(db.adminCommand({setQuerySettings: query, settings: settings}));

    // Wait until the settings have taken effect.
    const expectedConfiguration = [qsutils.makeQueryShapeConfiguration(settings, query)];
    qsutils.assertQueryShapeConfiguration(expectedConfiguration);

    // Then, check that the query used the appropriate engine and the hinted index.
    const {$db: _, ...queryWithoutDollarDb} = query;
    const explain = assert.commandWorked(db.runCommand({explain: queryWithoutDollarDb}));
    const engine = getEngine(explain)
    assert.eq(
        engine, expectedEngine, `Expected engine to be ${expectedEngine} but found ${engine}`);
    assertHintedIndexWasUsed(explain);
    qsutils.removeAllQuerySettings();
}

testQueryFramework({
    query: sbeEligibleQuery,
    settings: {queryFramework: "classic"},
    expectedEngine: "classic",
});

testQueryFramework({
    query: sbeEligibleQuery,
    settings: {queryFramework: "sbe"},
    expectedEngine: "sbe",
});

testQueryFramework({
    query: nonSbeEligibleQuery,
    settings: {queryFramework: "classic"},
    expectedEngine: "classic",
});

testQueryFramework({
    query: nonSbeEligibleQuery,
    settings: {queryFramework: "sbe"},
    expectedEngine: "classic",
});
