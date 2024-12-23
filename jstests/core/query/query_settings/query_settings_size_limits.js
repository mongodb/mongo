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

// Define some valid query settings we are going to apply to all queries.
const irrelevantQuerySettings = {
    indexHints: {ns: {db: dbName, coll: collName}, allowedIndexes: ["a123_1", {$natural: 1}]}
};

// Generate a number of larger disjunctive queries.
const $orArgumentCount = 30000;
const fieldNamePrefixes = Array.from("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
const largeQueries = fieldNamePrefixes.map(
    prefix => ({
        find: collName,
        filter: {$or: Array.from({length: $orArgumentCount}).map((p, i) => ({[prefix + i]: 1}))}
    }));

// Define the number of query settings fitting into 16MB (obtained experimentally).
const splitIndex = 20;

const coll = assertDropAndRecreateCollection(db, collName);
const qsutils = new QuerySettingsUtils(db, collName);

coll.insertOne({a123: 456});
coll.createIndexes([{a123: 1}]);

// Reset query settings.
qsutils.removeAllQuerySettings();

// Executing one large query without query settings should succeed.
assert.commandWorked(db.runCommand(largeQueries[0]));

// Setting query settings for the first 'splitIndex' queries should succeed.
largeQueries.slice(0, splitIndex)
    .forEach(query => assert.commandWorked(db.adminCommand(
                 {setQuerySettings: {...query, $db: dbName}, settings: irrelevantQuerySettings})));

// Re-running one large query with the persistent query settings should also succeed.
assert.commandWorked(db.runCommand(largeQueries[0]));

// Retrieving large query settings without debug query shapes should succeed.
const qsWithoutDebugShape = qsutils.getQuerySettings({showQueryShapeHash: true});

jsTest.log("Query settings size without debug query shapes (bytes): " +
           qsWithoutDebugShape.reduce((res, qs) => res + Object.bsonsize(qs), 0));

// Retrieving large query settings with debug query shapes should succeed. The total size is above
// 16MB, but each batch is under 16MB.
const qsWithDebugShape =
    qsutils.getQuerySettings({showQueryShapeHash: true, showDebugQueryShape: true});

jsTest.log("Query settings size with debug query shapes (bytes): " +
           qsWithDebugShape.reduce((res, qs) => res + Object.bsonsize(qs), 0));

// Setting query settings for the remaining queries should fail with the BSONObjectTooLarge error.
const failureIndex =
    largeQueries.slice(splitIndex).findIndex(query => db.adminCommand({
                                                            setQuerySettings:
                                                                {...query, $db: dbName},
                                                            settings: irrelevantQuerySettings
                                                        }).code === ErrorCodes.BSONObjectTooLarge);
assert.gt(failureIndex,
          -1,
          "expected 'setQuerySettings' command to fail with 'BSONObjectTooLarge' error");

// Clean-up at the end of the test.
qsutils.removeAllQuerySettings();
