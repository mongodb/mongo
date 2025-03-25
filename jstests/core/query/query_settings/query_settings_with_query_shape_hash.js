// Tests that setting PQS works with the 'queryShapeHash' exposed via $queryStats.
// @tags: [
//   # TODO SERVER-98659 Investigate why this test is failing on
//   # 'sharding_kill_stepdown_terminate_jscore_passthrough'
//   does_not_support_stepdowns,
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
//   requires_fcv_80,
// ]

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {
    getQueryStatsFindCmd,
    getQueryStatsShapeHashes,
} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();
const coll = assertDropAndRecreateCollection(db, collName);
const qsutils = new QuerySettingsUtils(db, collName);

// Enable query stats collection.
const originalQueryStatsRateLimit =
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryStatsRateLimit: -1}));

// Ensure that there are no query settings present at the start of the test.
qsutils.removeAllQuerySettings();

assert.commandWorked(coll.insertMany([
    {a: 1, b: 5},
    {a: 2, b: 4},
    {a: 3, b: 3},
    {a: 4, b: 2},
    {a: 5, b: 1},
]));

const querySettingsQuery = qsutils.makeFindQueryInstance({filter: {a: 1, b: 5}});
const query = qsutils.withoutDollarDB(querySettingsQuery);
const initialSettings = {
    queryFramework: "classic"
};
const finalSettings = {
    ...initialSettings,
    reject: true
};

// Run the find command to generate a query stats store entry and get its query shape hash.
assert.commandWorked(db.runCommand(query));
const entries = getQueryStatsFindCmd(db, {collName: collName});
const queryShapeHashes = getQueryStatsShapeHashes(entries);
assert.eq(queryShapeHashes.length, 1);

// Set query settings via hash. Representative query is missing so we can't run explain yet.
assert.commandWorked(
    db.adminCommand({setQuerySettings: queryShapeHashes[0], settings: initialSettings}));
qsutils.assertQueryShapeConfiguration([{settings: initialSettings}], false /* shouldRunExplain */);

// Update the query settings via query. Representative query will be populated and we can run
// explain to make sure that the settings applied with the hash map to and apply to the correct
// query shape.
assert.commandWorked(
    db.adminCommand({setQuerySettings: querySettingsQuery, settings: {reject: true}}));
qsutils.assertQueryShapeConfiguration(
    [qsutils.makeQueryShapeConfiguration(finalSettings, querySettingsQuery)]);

// Cleanup.
qsutils.removeAllQuerySettings();
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQueryStatsRateLimit: originalQueryStatsRateLimit.was}));