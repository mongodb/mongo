/**
 * Tests that index building is properly blocked and/or aborted during migrations.
 *
 * TODO (SERVER-63517): Replace tenant_migration_buildindex.js with this test.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   featureFlagShardMerge,
 *   requires_fcv_53,
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

// Index builds should be blocked by the tenant access blocker, not maxNumActiveUserIndexBuilds.
const tenantMigrationTest = new TenantMigrationTest(
    {name: jsTestName(), sharedOptions: {setParameter: {maxNumActiveUserIndexBuilds: 100}}});

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const kTenant1Id = "testTenantId1";
const kTenant2Id = "testTenantId2";
const kTenant1DbName = tenantMigrationTest.tenantDB(kTenant1Id, "testDB");
const kTenant2DbName = tenantMigrationTest.tenantDB(kTenant2Id, "testDB");
const kEmptyCollName = "testEmptyColl";
const kNonEmptyCollName = "testNonEmptyColl";
const kNewCollName1 = "testNewColl1";
const kNewCollName2 = "testNewColl2";

// Attempts to create an index on a collection and checks that it fails because a migration
// committed.
function createIndexShouldFail(primaryHost, dbName, collName, indexSpec) {
    const donorPrimary = new Mongo(primaryHost);
    const db = donorPrimary.getDB(dbName);
    const res = db[collName].createIndex(indexSpec);
    jsTestLog(`createIndex ${dbName}.${collName} spec: ${tojson(indexSpec)} reply: ${tojson(res)}`);
    assert.commandFailedWithCode(
        res, ErrorCodes.TenantMigrationCommitted, `${dbName.collName} ${tojson(indexSpec)}`);
}

const migrationId = UUID();
// TODO (SERVER-63454): remove tenantId, and remove kTenant2DbName, db2, tenant2IndexThread, etc.
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: kTenant1Id,
};
const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());

// Put some data in the non-empty collections, and create the empty one.
const db1 = donorPrimary.getDB(kTenant1DbName);
const db2 = donorPrimary.getDB(kTenant2DbName);
assert.commandWorked(db1[kNonEmptyCollName].insert([{a: 1, b: 1}, {a: 2, b: 2}, {a: 3, b: 3}]));
assert.commandWorked(db2[kNonEmptyCollName].insert([{x: 1, y: 1}, {x: 2, b: 2}, {x: 3, y: 3}]));
assert.commandWorked(db1.createCollection(kEmptyCollName));
assert.commandWorked(db2.createCollection(kEmptyCollName));

// Start index builds and have them hang in the builder thread.
var initFpCount =
    assert
        .commandWorked(donorPrimary.adminCommand(
            {configureFailPoint: "hangAfterInitializingIndexBuild", mode: "alwaysOn"}))
        .count;
const tenant1IndexThread =
    new Thread(createIndexShouldFail, donorPrimary.host, kTenant1DbName, kNonEmptyCollName, {b: 1});
// Even though tenantId1 is passed to donorStartMigration, the donor aborts this index too
// because protocol is "shard merge".
// TODO (SERVER-63454): remove comment above.
const tenant2IndexThread =
    new Thread(createIndexShouldFail, donorPrimary.host, kTenant2DbName, kNonEmptyCollName, {y: 1});
tenant1IndexThread.start();
tenant2IndexThread.start();
assert.commandWorked(donorPrimary.adminCommand({
    waitForFailPoint: "hangAfterInitializingIndexBuild",
    timesEntered: initFpCount + 2,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Start an index build and pause it after acquiring a slot but before registering itself.
const indexBuildSlotFp = configureFailPoint(donorPrimary, "hangAfterAcquiringIndexBuildSlot");
jsTestLog("Starting the racy index build");
const racyIndexThread1 =
    new Thread(createIndexShouldFail, donorPrimary.host, kTenant1DbName, kNonEmptyCollName, {a: 1});
const racyIndexThread2 =
    new Thread(createIndexShouldFail, donorPrimary.host, kTenant2DbName, kNonEmptyCollName, {a: 1});
racyIndexThread1.start();
racyIndexThread2.start();
indexBuildSlotFp.wait({timesEntered: 2});

jsTestLog("Starting a migration and pausing after majority-committing the initial state doc.");
// Start a migration, and pause it after the donor has majority-committed the initial state doc.
const dataSyncFp =
    configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingDataSyncState");
const migrationThread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
migrationThread.start();
dataSyncFp.wait();

// Release the previously-started index build thread and allow the donor to abort index builds
assert.commandWorked(donorPrimary.adminCommand(
    {configureFailPoint: "hangAfterInitializingIndexBuild", mode: "off"}));

// Release the racy thread; it should block.
indexBuildSlotFp.off();

// Should be able to create an index on a non-existent collection.  Since the collection is
// guaranteed to be empty and to have always been empty, this is safe.
assert.commandWorked(db1[kNewCollName1].createIndex({a: 1}));

// Attempts to create indexes on existing collections should fail.
const emptyIndexThread1 =
    new Thread(createIndexShouldFail, donorPrimary.host, kTenant1DbName, kEmptyCollName, {a: 1});
const emptyIndexThread2 =
    new Thread(createIndexShouldFail, donorPrimary.host, kTenant2DbName, kEmptyCollName, {a: 1});
emptyIndexThread1.start();
emptyIndexThread2.start();
const nonEmptyIndexThread1 =
    new Thread(createIndexShouldFail, donorPrimary.host, kTenant1DbName, kNonEmptyCollName, {c: 1});
const nonEmptyIndexThread2 =
    new Thread(createIndexShouldFail, donorPrimary.host, kTenant2DbName, kNonEmptyCollName, {c: 1});
nonEmptyIndexThread1.start();
nonEmptyIndexThread2.start();

jsTestLog("Allowing migration to commit");
// Allow the migration to move to the blocking state and commit.
dataSyncFp.off();
assert.soon(() => {
    const state =
        tenantMigrationTest
            .getTenantMigrationAccessBlocker({donorNode: donorPrimary, tenantId: kTenant1Id})
            .donor.state;
    return state === TenantMigrationTest.DonorAccessState.kBlockWritesAndReads ||
        state === TenantMigrationTest.DonorAccessState.kReject;
});
TenantMigrationTest.assertCommitted(migrationThread.returnData());

// The index creation threads should be done.
racyIndexThread1.join();
racyIndexThread2.join();
tenant1IndexThread.join();
tenant2IndexThread.join();
emptyIndexThread1.join();
emptyIndexThread2.join();
nonEmptyIndexThread1.join();
nonEmptyIndexThread2.join();

// Should not be able to create an index on any collection.
assert.commandFailedWithCode(db1[kEmptyCollName].createIndex({d: 1}),
                             ErrorCodes.TenantMigrationCommitted);
assert.commandFailedWithCode(db2[kEmptyCollName].createIndex({d: 1}),
                             ErrorCodes.TenantMigrationCommitted);
assert.commandFailedWithCode(db1[kNonEmptyCollName].createIndex({d: 1}),
                             ErrorCodes.TenantMigrationCommitted);
assert.commandFailedWithCode(db2[kNonEmptyCollName].createIndex({d: 1}),
                             ErrorCodes.TenantMigrationCommitted);
// Creating an index on a non-existent collection should fail because we can't create the
// collection, but it's the same error code.
assert.commandFailedWithCode(db1[kNewCollName2].createIndex({d: 1}),
                             ErrorCodes.TenantMigrationCommitted);
assert.commandFailedWithCode(db2[kNewCollName2].createIndex({d: 1}),
                             ErrorCodes.TenantMigrationCommitted);

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
})();
