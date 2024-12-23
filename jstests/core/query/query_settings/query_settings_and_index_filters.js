// Tests that query settings have higher priority than index filters. Once query settings are set,
// index filters for the given query are ignored. When query settings are removed, index filters are
// applied again.
// @tags: [
//   # TODO SERVER-98659 Investigate why this test is failing on
//   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
//   does_not_support_stepdowns,
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

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    getPlanStages,
    getQueryPlanners,
    getWinningPlanFromExplain
} from "jstests/libs/query/analyze_plan.js";
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

/**
 * Asserts that 'queryPlanner' contains index scan stages with the 'expectedIndex'.
 */
function assertIndexScan(queryPlanner, expectedIndex) {
    const ixscans = getPlanStages(getWinningPlanFromExplain(queryPlanner), "IXSCAN");
    assert.gte(ixscans.length, 1, queryPlanner);

    for (const stage of ixscans) {
        assert.docEq(stage.keyPattern, expectedIndex, queryPlanner);
    }
}

/**
 * Runs the explain over 'findCmd' and asserts index filter, query settings and indexes being used.
 */
function assertExplain(findCmd, {indexFilterSet, querySettings, expectedIndexScan}) {
    const explain = db.runCommand({explain: findCmd});

    getQueryPlanners(explain).forEach((queryPlanner) => {
        assert.eq(queryPlanner.indexFilterSet, indexFilterSet, queryPlanner);
        qsutils.assertEqualSettings(queryPlanner.querySettings, querySettings, queryPlanner);
        assertIndexScan(queryPlanner, expectedIndexScan);
    });
}

const querySettingsQuery = qsutils.makeFindQueryInstance({filter: {a: 1, b: 5}});
const query = qsutils.withoutDollarDB(querySettingsQuery);

// Ensure no index filters and no query settings are used if not previously set.
assertExplain(query, {indexFilterSet: false, querySettings: undefined, expectedIndexScan: indexAB});

// Set index filters and ensure they are used for the given 'query'.
assert.commandWorked(
    db.runCommand({planCacheSetFilter: coll.getName(), query: query.filter, indexes: [indexA]}));
assertExplain(query, {indexFilterSet: true, querySettings: undefined, expectedIndexScan: indexA});

// Ensure that if query settings are set, index filters are ignored and query settings are used
// instead.
const settings = {
    indexHints: {ns: {db: db.getName(), coll: coll.getName()}, allowedIndexes: [indexB]}
};
qsutils.withQuerySettings(querySettingsQuery, settings, () => {
    assertExplain(query,
                  {indexFilterSet: false, querySettings: settings, expectedIndexScan: indexB});
});

// Ensure that once query settings are removed, index filters are used again .
assertExplain(query, {indexFilterSet: true, querySettings: undefined, expectedIndexScan: indexA});

// Ensure no index filters are used, once all are cleared.
assert.commandWorked(db.runCommand({planCacheClearFilters: coll.getName()}));
assertExplain(query, {indexFilterSet: false, querySettings: undefined, expectedIndexScan: indexAB});
