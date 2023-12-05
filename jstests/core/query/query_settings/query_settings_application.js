// Tests query settings are applied regardless of the query engine (SBE or classic).
// @tags: [
//   # $planCacheStats can not be run with specified read preferences/concerns.
//   assumes_read_preference_unchanged,
//   assumes_read_concern_unchanged,
//   # $planCacheStats can not be run in transactions.
//   does_not_support_transactions,
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   simulate_atlas_proxy_incompatible,
//   cqf_incompatible,
//   # 'planCacheClear' command is not allowed with the security token.
//   not_allowed_with_security_token,
// ]
//

import {getPlanStages, getWinningPlan} from "jstests/libs/analyze_plan.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";
import {checkSBEEnabled} from "jstests/libs/sbe_util.js";

const isSBE = checkSBEEnabled(db);

// Create the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());
assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}, {a: 1, b: 1}]));

// Insert data into the collection.
assert.commandWorked(coll.insertMany([
    {a: 1, b: 5},
    {a: 2, b: 4},
    {a: 3, b: 3},
    {a: 4, b: 2},
    {a: 5, b: 1},
]))

const query = {
    find: coll.getName(),
    filter: {a: 1, b: 1},
    // The skip-clause is a part of the query shape, however, it is not propagated to the shards in
    // a sharded cluster. Nevertheless, the shards should use the query settings matching the
    // original query shape.
    skip: 3,
};
const querySettingsQuery = qsutils.makeFindQueryInstance({filter: {a: 1, b: 1}, skip: 3});
const querySettingsA = {
    indexHints: {allowedIndexes: ["a_1"]}
};
const querySettingsB = {
    indexHints: {allowedIndexes: ["b_1"]}
};
const querySettingsAB = {
    indexHints: {allowedIndexes: ["a_1_b_1"]}
};

// Ensure that query settings cluster parameter is empty.
qsutils.assertQueryShapeConfiguration([]);

// Ensure that explain output contains index scans with indexes specified in
// 'settings.indexHints.allowedIndexes'
function assertQuerySettingsApplication(findCmd, settings, shouldCheckPlanCache = true) {
    // Clear the plan cache before running any queries.
    coll.getPlanCache().clear();

    const explain = db.runCommand({explain: findCmd});
    const ixscanStages = getPlanStages(getWinningPlan(explain.queryPlanner), "IXSCAN");
    assert.gte(ixscanStages.length, 1, explain);
    const expectedIndexName = settings.indexHints.allowedIndexes[0];
    for (let ixscanStage of ixscanStages) {
        assert.docEq(ixscanStage.indexName, expectedIndexName, explain);
    }

    // Single solution plans are not cached in classic, therefore do not perform plan cache checks
    // for classic.
    if (!isSBE) {
        return;
    }

    if (!shouldCheckPlanCache) {
        return;
    }

    // Run the query and ensure that new plan cache entry (or plan cache entries in sharded
    // clusters) have been created.
    const planCacheStatsBeforeRunningCmd =
        coll.aggregate([{$planCacheStats: {}}, {$sort: {timeOfCreation: -1}}]).toArray();
    assert.commandWorked(db.runCommand(findCmd));
    const planCacheStatsAfterRunningCmd =
        coll.aggregate([{$planCacheStats: {}}, {$sort: {timeOfCreation: -1}}]).toArray();
    assert.gt(planCacheStatsAfterRunningCmd.length, planCacheStatsBeforeRunningCmd.length, {
        planCacheStatsBefore: planCacheStatsBeforeRunningCmd,
        planCacheStatsAfter: planCacheStatsAfterRunningCmd
    })

    // Take the newest plan cache entry (based on 'timeOfCreation' sorting) and ensure that it
    // contains the 'settings'.
    assert.docEq(
        planCacheStatsAfterRunningCmd[0].querySettings, settings, planCacheStatsAfterRunningCmd);
}

// Ensure query settings are applied as expected in a straightforward scenario.
{
    for (let settings of [querySettingsA, querySettingsB, querySettingsAB]) {
        // Set query settings for a query to use 'settings.indexHints.allowedIndexes' index.
        assert.commandWorked(
            db.adminCommand({setQuerySettings: querySettingsQuery, settings: settings}));
        qsutils.assertQueryShapeConfiguration(
            [qsutils.makeQueryShapeConfiguration(settings, querySettingsQuery)]);
        assertQuerySettingsApplication(query, settings);
    }
    qsutils.removeAllQuerySettings();
}

// Ensure that the hint gets ignored when query settings for the particular query are set.
{
    assert.commandWorked(
        db.adminCommand({setQuerySettings: querySettingsQuery, settings: querySettingsAB}));
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration(querySettingsAB, querySettingsQuery)]);

    // Avoid checking plan cache entries, as no new plan cache entries were generated.
    assertQuerySettingsApplication(
        {...query, hint: "a_1"}, querySettingsAB, false /* shouldCheckPlanCache */);

    qsutils.removeAllQuerySettings();
}

// Ensure that users can not pass query settings to the commands explicitly.
{
    assert.commandFailedWithCode(db.runCommand({...query, querySettings: querySettingsAB}),
                                 [7746900, 7746901])
}
