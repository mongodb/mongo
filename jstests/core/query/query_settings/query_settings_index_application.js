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
//   not_allowed_with_signed_security_token,
// ]
//

import {getPlanStages, getWinningPlan} from "jstests/libs/analyze_plan.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

const isSBE = checkSbeFullyEnabled(db);

// Create the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());

assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}, {a: 1, b: 1}]));
const indexA = "a_1";
const indexB = "b_1";
const indexAB = "a_1_b_1";

// Insert data into the collection.
assert.commandWorked(coll.insertMany([
    {a: 1, b: 5},
    {a: 2, b: 4},
    {a: 3, b: 3},
    {a: 4, b: 2},
    {a: 5, b: 1},
]));

// Ensure that query settings cluster parameter is empty.
qsutils.assertQueryShapeConfiguration([]);

function assertPlanCacheEntries(cmd, settings) {
    // Single solution plans are not cached in classic, therefore do not perform plan cache checks
    // for classic.
    if (!isSBE) {
        return;
    }

    // Take the newest plan cache entry (based on 'timeOfCreation' sorting) and ensure that it
    // contains the 'settings'.
    assert.commandWorked(db.runCommand(cmd));
    const planCacheStatsAfterRunningCmd =
        coll.aggregate([{$planCacheStats: {}}, {$sort: {timeOfCreation: -1}}]).toArray();
    assert.docEq(
        planCacheStatsAfterRunningCmd[0].querySettings, settings, planCacheStatsAfterRunningCmd);
}

function assertIndexScanStage(cmd, expectedIndex) {
    const explain = assert.commandWorked(db.runCommand({explain: cmd}));
    const winningPlan = getWinningPlan(explain.queryPlanner);
    const ixscanStages = getPlanStages(winningPlan, "IXSCAN");
    assert.gte(ixscanStages.length, 1, explain);
    for (const ixscanStage of ixscanStages) {
        assert.docEq(ixscanStage.indexName, expectedIndex, explain);
    }
}

function assertCollScanStage(cmd, allowedDirections) {
    const explain = assert.commandWorked(db.runCommand({explain: cmd}));
    const winningPlan = getWinningPlan(explain.queryPlanner);
    const collscanStages = getPlanStages(winningPlan, "COLLSCAN");
    assert.gte(collscanStages.length, 1, explain);
    for (const collscanStage of collscanStages) {
        assert(allowedDirections.includes(collscanStage.direction), explain);
    }
}

// Helper method for setting & removing query settings for testing purposes. Accepts a 'runTest'
// anonymous function which will be executed once the provided query settings have been propagated
// throughout the cluster.
function withQuerySettings(representativeQuery, settings, runTest) {
    assert.commandWorked(
        db.adminCommand({setQuerySettings: representativeQuery, settings: settings}));
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration(settings, representativeQuery)]);
    // Clear the plan cache before running any queries.
    coll.getPlanCache().clear();
    runTest();
    qsutils.removeAllQuerySettings();
}

// Ensure query settings are applied as expected in a straightforward scenario.
function testQuerySettingsIndexApplication(query, querySettingsQuery) {
    for (const index of [indexA, indexB, indexAB]) {
        const settings = {indexHints: {allowedIndexes: [index]}};
        withQuerySettings(querySettingsQuery, settings, () => {
            assertIndexScanStage(query, index);
            assertPlanCacheEntries(query, settings);
        });
    }
}

// Ensure query settings '$natural' hints are applied as expected in a straightforward scenario.
// This test case covers the following scenarios:
//     * Only forward scans allowed.
//     * Only backward scans allowed.
//     * Both forward and backward scans allowed.
function testQuerySettingsNaturalApplication(query, querySettingsQuery) {
    const naturalForwardScan = {$natural: 1};
    const naturalForwardSettings = {indexHints: {allowedIndexes: [naturalForwardScan]}};
    withQuerySettings(querySettingsQuery, naturalForwardSettings, () => {
        assertCollScanStage(query, ["forward"]);
        assertPlanCacheEntries(query, naturalForwardSettings);
    });

    const naturalBackwardScan = {$natural: -1};
    const naturalBackwardSettings = {indexHints: {allowedIndexes: [naturalBackwardScan]}};
    withQuerySettings(querySettingsQuery, naturalBackwardSettings, () => {
        assertCollScanStage(query, ["backward"]);
        assertPlanCacheEntries(query, naturalBackwardSettings);
    });

    const naturalAnyDirectionSettings = {
        indexHints: {allowedIndexes: [naturalForwardScan, naturalBackwardScan]}
    };
    withQuerySettings(querySettingsQuery, naturalAnyDirectionSettings, () => {
        assertCollScanStage(query, ["forward", "backward"]);
        assertPlanCacheEntries(query, naturalAnyDirectionSettings);
    });
}

// Ensure that the hint gets ignored when query settings for the particular query are set.
function testQuerySettingsIgnoreCursorHints(query, querySettingsQuery) {
    const settings = {indexHints: {allowedIndexes: [indexAB]}};
    const queryWithHint = {...query, hint: indexA};
    withQuerySettings(querySettingsQuery, settings, () => {
        // Avoid checking plan cache entries, as no new plan cache entries were generated.
        assertIndexScanStage(queryWithHint, indexAB);
    });
}

// Ensure that queries fallback to multiplanning when the provided settings don't generate any
// viable plans. Limit the query to an index which does not exist and expect it to use 'A_B'.
function testQuerySettingsFallback(query, querySettingsQuery) {
    const settings = {indexHints: {allowedIndexes: ["doesnotexist"]}};
    withQuerySettings(querySettingsQuery, settings, () => {
        assertIndexScanStage(query, indexAB);
        assertPlanCacheEntries(query, settings);
    });
}

// Ensure that users can not pass query settings to the commands explicitly.
function testQuerySettingsCommandValidation(query) {
    const settings = {indexHints: {allowedIndexes: [indexAB]}};
    const expectedErrorCodes = [7746900, 7746901, 7923000, 7923001, 7708000, 7708001];
    assert.commandFailedWithCode(db.runCommand({...query, querySettings: settings}),
                                 expectedErrorCodes);
}

(function testFindQuerySettingsApplication() {
    const findQuery = {
        find: coll.getName(),
        filter: {a: 1, b: 1},
        // The skip-clause is a part of the query shape, however, it is not propagated to the shards
        // in a sharded cluster. Nevertheless, the shards should use the query settings matching the
        // original query shape.
        skip: 3,
    };
    const querySettingsFindQuery = qsutils.makeFindQueryInstance({
        filter: {a: 1, b: 1},
        skip: 3,
    });
    testQuerySettingsIndexApplication(findQuery, querySettingsFindQuery);
    testQuerySettingsNaturalApplication(findQuery, querySettingsFindQuery);
    testQuerySettingsIgnoreCursorHints(findQuery, querySettingsFindQuery);
    testQuerySettingsFallback(findQuery, querySettingsFindQuery);
    testQuerySettingsCommandValidation(findQuery);
})();

(function testDistinctQuerySettingsApplication() {
    const distinctQuery = {distinct: coll.getName(), key: 'c', query: {a: 1, b: 1}};
    const querySettingsDistinctQuery = qsutils.makeDistinctQueryInstance({
        key: 'c',
        query: {a: 1, b: 1},
    });
    testQuerySettingsIndexApplication(distinctQuery, querySettingsDistinctQuery);
    testQuerySettingsNaturalApplication(distinctQuery, querySettingsDistinctQuery);
    testQuerySettingsIgnoreCursorHints(distinctQuery, querySettingsDistinctQuery);
    testQuerySettingsFallback(distinctQuery, querySettingsDistinctQuery);
    testQuerySettingsCommandValidation(distinctQuery);
})();

(function testAggregateQuerySettingsApplication() {
    const aggregateQuery = {
        aggregate: coll.getName(),
        pipeline: [{$match: {a: 1}}],
        cursor: {},
    };
    testQuerySettingsCommandValidation(aggregateQuery);
})();
