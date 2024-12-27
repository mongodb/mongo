// Tests that even when plan cache entry for a given query exists, a new one is created (and used)
// once query settings are set for the corresponding query. The old plan cache is used once again,
// when query settings are removed.
// @tags: [
//   # TODO SERVER-98659 Investigate why this test is failing on
//   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
//   does_not_support_stepdowns,
//   # Index filters are not replicated and therefore won't be applied on secondaries.
//   assumes_read_preference_unchanged,
//   assumes_read_concern_unchanged,
//   # SBE plan cache key includes information about the collection. If a balancer operation
//   # happens between where we compute a cache key using explain(), and where we look it up using
//   # $planCacheStats, the two operations will use different cache keys and the test will fail.
//   assumes_balancer_off,
//   # Query settings commands can not be run on the shards directly.
//   directly_against_shardsvrs_incompatible,
//   # Index filter commands do not accept security token.
//   not_allowed_with_signed_security_token,
//   requires_fcv_80,
//   # Query settings commands can not be handled by atlas proxy.
//   simulate_atlas_proxy_incompatible,
//   requires_getmore,
// ]

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {getPlanCacheKeyFromExplain} from "jstests/libs/query/analyze_plan.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());
qsutils.removeAllQuerySettings();

// Insert data into the collection.
assert.commandWorked(coll.insertMany([
    {a: 1, b: 5},
    {a: 2, b: 4},
    {a: 3, b: 3},
    {a: 4, b: 2},
    {a: 5, b: 1},
]));

// Create indexes.
const indexA = {
    a: 1
};
const indexB = {
    b: 1
};
const indexAB = {
    a: 1,
    b: 1
};
assert.commandWorked(coll.createIndexes([indexA, indexB, indexAB]));

function getAllPlanCacheEntries() {
    return coll.aggregate([{$planCacheStats: {}}]).toArray();
}

/**
 * Ensures that the plan cache for 'coll' is empyt
 */
function assertEmptyPlanCache() {
    const planCacheEntries = getAllPlanCacheEntries();
    assert.eq(0, planCacheEntries.length, planCacheEntries);
}

/**
 * Ensures that a single plan cache entry with the given 'planCacheKeyHash' exists and it has the
 * 'expectedQuerySettings'.
 */
function assertPlanCacheEntryWithQuerySettings(planCacheKeyHash, expectedQuerySettings) {
    const correspondingPlanCacheEntries =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: planCacheKeyHash}}])
            .toArray();
    assert.gte(correspondingPlanCacheEntries.length, 1, getAllPlanCacheEntries());
    correspondingPlanCacheEntries.forEach(entry => {
        qsutils.assertEqualSettings(expectedQuerySettings, entry.querySettings, entry);
    });
}

/**
 * Runs the query, asserts that the plan cache entry contains the 'expectedQuerySettings' as well as
 * returns the corresponding planCacheKeyHash.
 */
function runQueryAndAssertPlanCache(expectedQuerySettings) {
    assert.commandWorked(db.runCommand(query));
    const planCacheKeyHash = getPlanCacheKeyFromExplain(db.runCommand({explain: query}));
    assertPlanCacheEntryWithQuerySettings(planCacheKeyHash, expectedQuerySettings);
    return planCacheKeyHash;
}

const querySettingsQuery = qsutils.makeFindQueryInstance({filter: {a: 1, b: 5}});
const query = qsutils.withoutDollarDB(querySettingsQuery);

assertEmptyPlanCache();

// Run query and ensure the corresponding plan cache entry exists and query settings are not
// present.
runQueryAndAssertPlanCache(undefined /* expectedQuerySettings */);

// Ensure that if query settings are set, and 'query' is executed, a new plan cache entry will be
// created and query settings will be present.
const settings = {
    indexHints: {ns: {db: db.getName(), coll: coll.getName()}, allowedIndexes: [indexA, indexB]}
};
const planCacheKeyHashWithQuerySettings = qsutils.withQuerySettings(
    querySettingsQuery, settings, () => runQueryAndAssertPlanCache(settings));

// Ensure that once query settings are removed, a different plan cache entry is used.
const planCacheKeyHashWithoutQuerySettings =
    runQueryAndAssertPlanCache(undefined /* expectedQuerySettings */);
assert.neq(planCacheKeyHashWithoutQuerySettings, planCacheKeyHashWithQuerySettings);
