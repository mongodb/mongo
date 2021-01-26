/**
 * Tests that tenant migration donor cleans up index build correctly if it fails to write
 * commitIndexBuild oplog entry due to TenantMigrationConflict or TenantMigrationCommitted.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

// TODO SERVER-48862: Remove 'enableRecipientTesting: false'.
const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), enableRecipientTesting: false});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    tenantMigrationTest.stop();
    return;
}

const kTenantId = "testTenantId";
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const kCollName = "testColl";
const kIndex = {
    key: {x: 1},
    name: "x_1",
};

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const testDB = donorPrimary.getDB(kDbName);

/**
 * Runs createIndex command on the given primary host, asserts that it fails with
 * TenantMigrationCommitted error.
 */
function createIndex(primaryHost, dbName, collName, index) {
    const donorPrimary = new Mongo(primaryHost);
    assert.commandFailedWithCode(
        donorPrimary.getDB(dbName).runCommand({createIndexes: collName, indexes: [index]}),
        ErrorCodes.TenantMigrationCommitted);
}

// Insert a doc since two-phase index build is only used for non-empty collections.
assert.commandWorked(testDB.runCommand({insert: kCollName, documents: [{x: 1}]}));

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: kTenantId,
};
const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());

// Start a migration, and pause it after the donor has majority-committed the initial state doc.
const dataSyncFp =
    configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingDataSyncState");
const migrationThread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
migrationThread.start();
dataSyncFp.wait();

// Run a two-phase index build, and pause it right before it commits.
const hangIndexBuildFp = configureFailPoint(donorPrimary, "hangIndexBuildBeforeCommit");
const indexBuildThread = new Thread(createIndex, donorPrimary.host, kDbName, kCollName, kIndex);
indexBuildThread.start();
hangIndexBuildFp.wait();

// Allow the migration to move to the blocking state and commit.
dataSyncFp.off();
assert.soon(() => {
    const state =
        tenantMigrationTest.getTenantMigrationAccessBlocker(donorPrimary, kTenantId).state;
    return state === TenantMigrationTest.AccessState.kBlockWritesAndReads ||
        state === TenantMigrationTest.AccessState.kReject;
});
hangIndexBuildFp.off();
assert.commandWorked(migrationThread.returnData());
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

// Verify that the index build failed and cleaned up correctly.
indexBuildThread.join();
const res = assert.commandWorked(testDB.runCommand({listIndexes: kCollName}));
assert(res.cursor.firstBatch.every(index => bsonWoCompare(index.key, kIndex.key) != 0));

const cmdNs = testDB.getCollection('$cmd').getFullName();
const ops = tenantMigrationTest.getDonorRst().dumpOplog(
    donorPrimary, {op: 'c', ns: cmdNs, 'o.abortIndexBuild': kCollName});
assert.eq(1, ops.length, 'primary did not write abortIndexBuild oplog entry: ' + tojson(ops));

tenantMigrationTest.stop();
})();
