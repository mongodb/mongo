/**
 * Tests that tenant migrations aborts without crashing when a donor collection is renamed.
 *
 * TODO SERVER-61231: shard merge does not use collection cloner, so we need another way
 * to pause the migration at the correct time. What should shard merge behavior be for
 * renaming a collection while a migration is underway? adapt this test
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_fcv_52,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

function insertData(collection) {
    // Enough for several batches.
    const bigStr = Array(1025).toString();  // 1KB of ','
    for (let i = 0; i < 1500; i++) {
        assert.commandWorked(collection.insert({bigStr: bigStr}));
    }
}

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
const kTenantId = "testTenantId";
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const kCollectionName = "toBeRenamed";
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
const db = donorPrimary.getDB(kDbName);

jsTestLog("Populate collection");
insertData(db[kCollectionName]);

jsTestLog("Done populating collection");
const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: kTenantId,
};

const fpAfterBatch = configureFailPoint(
    recipientPrimary, "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse");

jsTestLog("Start a migration and pause after first batch");
const migrationThread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
migrationThread.start();

jsTestLog("Wait to reach failpoint");
fpAfterBatch.wait();

jsTestLog("Rename collection");
assert.commandWorked(db.adminCommand({
    renameCollection: db[kCollectionName].getFullName(),
    to: db[kCollectionName].getFullName() + "Renamed"
}));

jsTestLog("Let migration abort");
fpAfterBatch.off();
TenantMigrationTest.assertAborted(migrationThread.returnData(), ErrorCodes.DuplicateKey);
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.stop();
})();
