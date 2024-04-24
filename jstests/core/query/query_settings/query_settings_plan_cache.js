// Tests that even when plan cache entry for a given query exists, a new one is created (and used)
// once query settings are set for the corresponding query. The old plan cache is used once again,
// when query settings are removed.
// @tags: [
//   # Index filters are not replicated and therefore won't be applied on secondaries.
//   assumes_read_preference_unchanged,
//   assumes_read_concern_unchanged,
//   # Query settings commands can not be run on the shards directly.
//   directly_against_shardsvrs_incompatible,
//   # Index filter commands do not accept security token.
//   not_allowed_with_signed_security_token,
//   requires_fcv_80,
//   # Query settings commands can not be handled by atlas proxy.
//   simulate_atlas_proxy_incompatible,
// ]

import {getPlanCacheKeyFromExplain} from "jstests/libs/analyze_plan.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

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

const querySettingsQuery = qsutils.makeFindQueryInstance({filter: {a: 1, b: 5}});
const query = qsutils.withoutDollarDB(querySettingsQuery);

assertEmptyPlanCache();

// Run query and ensure the corresponding plan cache entry exists and query settings are not
// present.
assert.commandWorked(db.runCommand(query));
const planCacheKeyHashWithoutQuerySettings =
    getPlanCacheKeyFromExplain(db.runCommand({explain: query}));
assertPlanCacheEntryWithQuerySettings(planCacheKeyHashWithoutQuerySettings,
                                      undefined /* querySettings */);

// Ensure that if query settings are set, and 'query' is executed, a new plan cache entry will be
// created and query settings will be present.
const settings = {
    indexHints: {ns: {db: db.getName(), coll: coll.getName()}, allowedIndexes: [indexA, indexB]}
};
qsutils.withQuerySettings(querySettingsQuery, settings, () => {
    assert.commandWorked(db.runCommand(query));

    const planCacheKeyHashWithQuerySettings =
        getPlanCacheKeyFromExplain(db.runCommand({explain: query}));
    assert.neq(planCacheKeyHashWithQuerySettings, planCacheKeyHashWithoutQuerySettings);
    assertPlanCacheEntryWithQuerySettings(planCacheKeyHashWithQuerySettings, settings);
});

// Ensure that once query settings are removed, the previously created plan cache entry will be
// used.
assert.eq(getPlanCacheKeyFromExplain(db.runCommand({explain: query})),
          planCacheKeyHashWithoutQuerySettings);
assertPlanCacheEntryWithQuerySettings(planCacheKeyHashWithoutQuerySettings,
                                      undefined /* querySettings */);
