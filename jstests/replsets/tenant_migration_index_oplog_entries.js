/**
 * Tests that we don't throw an error when the client performs two-phase index build operations,
 * or inserts docs that contain "commitIndexBuild" or "abortIndexBuild" fields.
 * @tags: [requires_fcv_47, incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kDbName = "testDb";
const kCollName = "testColl";
const kNs = kDbName + "." + kCollName;

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
if (!TenantMigrationUtil.isFeatureFlagEnabled(rst.getPrimary())) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    rst.stopSet();
    return;
}

const primary = rst.getPrimary();
const testDB = primary.getDB(kDbName);
const testColl = primary.getCollection(kNs);

assert.commandWorked(testDB.createCollection(kCollName));
assert.commandWorked(testDB.runCommand({insert: kCollName, documents: [{x: 0}]}));

jsTest.log("Test committing index build");
assert.commandWorked(testColl.createIndex({x: 1}));

jsTest.log("Test aborting index build");
let fp = configureFailPoint(primary, "failIndexBuildOnCommit");
assert.commandFailedWithCode(testColl.createIndex({y: 1}), 4698903);
fp.off();

jsTest.log("Test inserting docs that contain 'commitIndexBuild' and 'abortIndexBuild' fields");
assert.commandWorked(
    testDB.runCommand({insert: kCollName, documents: [{x: 1}, {commitIndexBuild: 1}]}));
assert.commandWorked(
    testDB.runCommand({insert: kCollName, documents: [{x: 1}, {abortIndexBuild: 1}]}));

jsTest.log("Test inserting docs that correspond to the 'o' field of valid 'commitIndexBuild' " +
           "and 'abortIndexBuild' oplog entries");
assert.commandWorked(testDB.runCommand({
    insert: kCollName,
    documents: [
        {x: 1},
        {
            commitIndexBuild: kCollName,
            indexBuildUUID: {$uuid: UUID()},
            indexes: [{v: 2, key: {z: 1.0}, name: "z_1"}],
            ts: {$timestamp: new Timestamp()},
            t: 1,
            wall: {$date: new Date()}
        }
    ]
}));
assert.commandWorked(testDB.runCommand({
    insert: kCollName,
    documents: [
        {x: 1},
        {
            abortIndexBuild: kCollName,
            indexBuildUUID: UUID(),
            indexes: [{v: 2, key: {z: 1.0}, name: "z_1"}],
            cause: {
                ok: false,
                code: 4698903,
                codeName: "Location4698903",
                errmsg: "index build aborted due to failpoint"
            },
            ts: {$timestamp: new Timestamp()},
            t: 1,
            wall: {$date: new Date()}
        }
    ]
}));

rst.stopSet();
})();
