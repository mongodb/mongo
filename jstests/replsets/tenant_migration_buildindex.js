/**
 * Tests that index building is properly blocked and/or aborted during migrations.
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
// committed.
function createIndexShouldFail(
    primaryHost, dbName, collName, indexSpec, errorCode = ErrorCodes.TenantMigrationCommitted) {
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

jsTestLog("Starting a migration and pausing after majority-committing the initial state doc.");
// Start a migration, and pause it after the donor has majority-committed the initial state doc.
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

// Attempts to create indexes on existing collections should fail.
const emptyIndexThread =
    new Thread(createIndexShouldFail, donorPrimary.host, kDbName, kEmptyCollName, {a: 1});
emptyIndexThread.start();
const nonEmptyIndexThread =
    new Thread(createIndexShouldFail, donorPrimary.host, kDbName, kNonEmptyCollName, {a: 1});
nonEmptyIndexThread.start();

jsTestLog("Allowing migration to commit");
// Allow the migration to move to the blocking state and commit.
dataSyncFp.off();
assert.soon(() => {
    const state =
        tenantMigrationTest.getTenantMigrationAccessBlocker(donorPrimary, kTenantId).state;
    return state === TenantMigrationTest.AccessState.kBlockWritesAndReads ||
        state === TenantMigrationTest.AccessState.kReject;
});
assert.commandWorked(migrationThread.returnData());

// The index creation threads should be done.
racyIndexThread.join();
emptyIndexThread.join();
nonEmptyIndexThread.join();

// Should not be able to create an index on any collection.
assert.commandFailedWithCode(db[kEmptyCollName].createIndex({b: 1}),
                             ErrorCodes.TenantMigrationCommitted);
assert.commandFailedWithCode(db[kNonEmptyCollName].createIndex({b: 1}),
                             ErrorCodes.TenantMigrationCommitted);
// Creating an index on a non-existent collection should fail because we can't create the
// collection, but it's the same error code.
assert.commandFailedWithCode(db[kNewCollName2].createIndex({b: 1}),
                             ErrorCodes.TenantMigrationCommitted);

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
})();
