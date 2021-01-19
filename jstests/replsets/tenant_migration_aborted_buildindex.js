/**
 * Tests that index building is properly completed when a migration aborts.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const kTenantId = "testTenantId";
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const kEmptyCollName = "testEmptyColl";
const kNonEmptyCollName = "testNonEmptyColl";
const kNewCollName1 = "testNewColl1";
const kNewCollName2 = "testNewColl2";

const donorPrimary = tenantMigrationTest.getDonorPrimary();

// Attempts to create an index on a collection and checks that it fails because a migration
// aborted.
function createIndexShouldFail(
    primaryHost, dbName, collName, indexSpec, errorCode = ErrorCodes.TenantMigrationAborted) {
    const donorPrimary = new Mongo(primaryHost);
    const db = donorPrimary.getDB(dbName);
    assert.commandFailedWithCode(db[collName].createIndex(indexSpec), errorCode);
}

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: kTenantId,
};
const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());

// Put some data in the non-empty collection, and create the empty one.
const db = donorPrimary.getDB(kDbName);
assert.commandWorked(db[kNonEmptyCollName].insert([{a: 1, b: 1}, {a: 2, b: 2}, {a: 3, b: 3}]));
assert.commandWorked(db.createCollection(kEmptyCollName));

// Start an index build and pause it after acquiring a slot but before registering itself.
const indexBuildFp = configureFailPoint(donorPrimary, "hangAfterAcquiringIndexBuildSlot");
jsTestLog("Starting the racy index build");
const racyIndexThread =
    new Thread(createIndexShouldFail, donorPrimary.host, kDbName, kNonEmptyCollName, {a: 1});
racyIndexThread.start();
indexBuildFp.wait();

// Start a migration, and pause it after the donor has majority-committed the initial state doc.
jsTestLog("Starting a migration and pausing after majority-committing the initial state doc.");
const dataSyncFp =
    configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingDataSyncState");
const migrationThread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
migrationThread.start();
dataSyncFp.wait();

// Release the racy thread; it should block.
indexBuildFp.off();

// Should be able to create an index on a non-existent collection.  Since the collection is
// guaranteed to be empty and to have always been empty, this is safe.
assert.commandWorked(db[kNewCollName1].createIndex({a: 1}));

// Clear the log so we can wait for the new index builds to start.
assert.commandWorked(donorPrimary.adminCommand({clearLog: "global"}));

// Attempts to create indexes on existing collections should block.
const emptyIndexThread1 =
    new Thread(createIndexShouldFail, donorPrimary.host, kDbName, kEmptyCollName, {a: 1});
emptyIndexThread1.start();
const nonEmptyIndexThread1 =
    new Thread(createIndexShouldFail, donorPrimary.host, kDbName, kNonEmptyCollName, {a: 1});
nonEmptyIndexThread1.start();

// Wait for both indexes to check tenant migration status.
assert.soon(() => checkLog.checkContainsWithCountJson(donorPrimary, 4886202, undefined, 2));

// Allow the migration to move to the blocking state.
const blockingFp =
    configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");
dataSyncFp.off();
assert.soon(
    () => tenantMigrationTest.getTenantMigrationAccessBlocker(donorPrimary, kTenantId).state ===
        TenantMigrationTest.AccessState.kBlockWritesAndReads);

// Clear the log so we can wait for the new index builds to start.
assert.commandWorked(donorPrimary.adminCommand({clearLog: "global"}));

// Attempts to create indexes on existing collections should still block.
const emptyIndexThread2 =
    new Thread(createIndexShouldFail, donorPrimary.host, kDbName, kEmptyCollName, {b: 1});
emptyIndexThread2.start();
const nonEmptyIndexThread2 =
    new Thread(createIndexShouldFail, donorPrimary.host, kDbName, kNonEmptyCollName, {b: 1});
nonEmptyIndexThread2.start();
// Wait for all indexes to check tenant migration status.
assert.soon(() => checkLog.checkContainsWithCountJson(donorPrimary, 4886202, undefined, 2));

// Allow the migration to abort.
const abortFp = configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
blockingFp.off();

assert.commandWorked(migrationThread.returnData());
abortFp.off();

// The index creation threads should be done.
racyIndexThread.join();
emptyIndexThread1.join();
nonEmptyIndexThread1.join();
emptyIndexThread2.join();
nonEmptyIndexThread2.join();

// Should be able to create an index on any collection.
assert.commandWorked(db[kEmptyCollName].createIndex({a: 1, b: 1}));
assert.commandWorked(db[kNonEmptyCollName].createIndex({a: 1, b: 1}));

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
})();
