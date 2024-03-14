/**
 * Tests that database commands related to persisted query settings fail gracefully when BSON object
 * size limit is exceeded.
 * @tags: [
 *   directly_against_shardsvrs_incompatible,
 *   requires_non_retryable_commands,
 *   simulate_atlas_proxy_incompatible,
 *   requires_fcv_80,
 *   # TODO SERVER-87047: re-enable test in suites with random migrations
 *   assumes_balancer_off,
 * ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

const dbName = db.getName();
const collName = jsTestName();
const ns = {
    db: dbName,
    coll: collName
};

const entryCount = 300000;
const largeFindQueryA = {
    find: collName,
    filter: {$or: Array.from({length: entryCount}).map((p, i) => ({["a" + i]: 1}))}
};
const largeFindQueryB = {
    find: collName,
    filter: {$or: Array.from({length: entryCount}).map((p, i) => ({["b" + i]: 1}))}
};

const coll = assertDropAndRecreateCollection(db, collName);
const qsutils = new QuerySettingsUtils(db, collName);

coll.insertOne({a123: 456});
coll.createIndexes([{a123: 1}]);

// Expect empty query settings at the beginning of the test.
qsutils.assertQueryShapeConfiguration([]);

// Running a large find query without query settings should succeed.
assert.commandWorked(db.runCommand(largeFindQueryA));

// Setting query settings for a large find query should succeed.
assert.commandWorked(db.adminCommand({
    setQuerySettings: {...largeFindQueryA, $db: dbName},
    settings: {indexHints: {ns, allowedIndexes: ["a123_1", {$natural: 1}]}}
}));

// Re-running the large find query with the persisted query settings should also succeed.
assert.commandWorked(db.runCommand(largeFindQueryA));

// Retrieving large query settings should succeed.
assert.commandWorked(db.adminCommand({aggregate: 1, pipeline: [{$querySettings: {}}], cursor: {}}));

// Explaining a large find query should fail.
assert.commandFailedWithCode(db.runCommand({explain: largeFindQueryA}),
                             ErrorCodes.BSONObjectTooLarge);

// Retrieving large query settings with debug query shape should fail with the BSONObjectTooLarge
// error.
assert.commandFailedWithCode(
    db.adminCommand(
        {aggregate: 1, pipeline: [{$querySettings: {showDebugQueryShape: true}}], cursor: {}}),
    ErrorCodes.BSONObjectTooLarge);

// Setting query settings for another large query should fail with the BSONObjectTooLarge error.
assert.commandFailedWithCode(db.adminCommand({
    setQuerySettings: {...largeFindQueryB, $db: dbName},
    settings: {indexHints: {ns, allowedIndexes: ["b1_1"]}}
}),
                             ErrorCodes.BSONObjectTooLarge);

// Clean-up at the end of the test.
qsutils.removeAllQuerySettings();
