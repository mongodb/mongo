/**
 * Tests that database commands related to persisted query settings fail gracefully when BSON object
 * size limit is exceeded.
 * @tags: [
 *   # TODO SERVER-98659 Investigate why this test is failing on
 *   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
 *   does_not_support_stepdowns,
 *   directly_against_shardsvrs_incompatible,
 *   requires_non_retryable_commands,
 *   simulate_atlas_proxy_incompatible,
 *   requires_fcv_80,
 *   # TODO SERVER-89461 Investigate why test using huge batch size timeout in suites with balancer.
 *   assumes_balancer_off,
 * ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const dbName = db.getName();
const collName = jsTestName();
const ns = {
    db: dbName,
    coll: collName
};
assertDropAndRecreateCollection(db, collName);

// SPM-3684 will store representative queries in the 'queryShapeRepresentativeQueries' collection,
// which makes 16MB limit of query settings harder to reach. Due to that, we will specify query
// settings with large index names in order to reach the limit.
const qsutils = new QuerySettingsUtils(db, collName);
qsutils.removeAllQuerySettings();

const queryA = qsutils.makeFindQueryInstance({filter: {a: "a"}});
const queryB = qsutils.makeFindQueryInstance({filter: {b: "b"}});
const querySettingsWithLargeIndexName = {
    indexHints: {ns, allowedIndexes: ["a".repeat(10 * 1024 * 1024)]}
};

// Specifying query settings with the same large index name should succed as total size of
// 'querySettings' cluster parameter is less than 16MB.
assert.commandWorked(
    db.adminCommand({setQuerySettings: queryA, settings: querySettingsWithLargeIndexName}));

// Specifying query settings with the same large index name should fail as total size of
// 'querySettings' cluster parameter exceeds 16MB.
assert.commandFailedWithCode(
    db.adminCommand({setQuerySettings: queryB, settings: querySettingsWithLargeIndexName}),
    ErrorCodes.BSONObjectTooLarge);

// Ensure that only a single query settings is present.
qsutils.assertQueryShapeConfiguration(
    [qsutils.makeQueryShapeConfiguration(querySettingsWithLargeIndexName, queryA)]);

// Specifying query settings with total size less than 16MB should still work.
assert.commandWorked(db.adminCommand({setQuerySettings: queryB, settings: {reject: true}}));

// Ensure that both query shape configurations are present.
qsutils.assertQueryShapeConfiguration([
    qsutils.makeQueryShapeConfiguration(querySettingsWithLargeIndexName, queryA),
    qsutils.makeQueryShapeConfiguration({reject: true}, queryB)
]);

// Perform query settings cleanup.
qsutils.removeAllQuerySettings();
