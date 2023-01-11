/**
 * Exercises the code path for the recipientSyncData command that waits until a timestamp provided
 * by the donor is majority committed: make sure that in this code path, when the recipient is
 * interrupted by a primary step down, the recipient properly swaps the error code to the true code
 * (like primary step down) that the donor can retry on.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_persistence,
 *   requires_replication,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");  // For extractUUIDFromObject()

// Make the batch size small so that we can pause before all the batches are applied.
const tenantMigrationTest = new TenantMigrationTest(
    {name: jsTestName(), sharedOptions: {setParameter: {tenantApplierBatchSizeOps: 2}}});

const kMigrationId = UUID();
const kTenantId = ObjectId().str;
const kReadPreference = {
    mode: "primary"
};
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(kMigrationId),
    tenantId: kTenantId,
    readPreference: kReadPreference
};

const dbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const collName = jsTestName() + "_collection";

const recipientRst = tenantMigrationTest.getRecipientRst();
const recipientPrimary = recipientRst.getPrimary();

// FailPoint to pause right before the data consistent promise is fulfilled.
const fpBeforeDataConsistent = configureFailPoint(
    recipientPrimary, "fpBeforeFulfillingDataConsistentPromise", {action: "hang"});
const fpBeforeApplierFutureCalled =
    configureFailPoint(recipientPrimary, "fpWaitUntilTimestampMajorityCommitted");

tenantMigrationTest.insertDonorDB(dbName, collName);

jsTestLog("Starting migration.");
// Start the migration, and allow it to progress to the point where the _dataConsistentPromise has
// been fulfilled.
tenantMigrationTest.startMigration(migrationOpts);

jsTestLog("Waiting for data consistent promise.");
// Pause right before the _dataConsistentPromise is fulfilled. Therefore, the applier has
// finished applying entries at least until dataConsistentStopDonorOpTime.
fpBeforeDataConsistent.wait();

jsTestLog("Pausing the tenant oplog applier.");
// Pause the applier now. All the entries that the applier cannot process now are past the
// dataConsistentStopDonorOpTime.
const fpPauseOplogApplier =
    configureFailPoint(recipientPrimary, "fpBeforeTenantOplogApplyingBatch");

jsTestLog("Writing to donor db.");
// Send writes to the donor. The applier will not be able to process these as it is paused.
const docsToApply = [...Array(10).keys()].map((i) => ({a: i}));
tenantMigrationTest.insertDonorDB(dbName, collName, docsToApply);

jsTestLog("Waiting to hit failpoint in tenant oplog applier.");
fpPauseOplogApplier.wait();

jsTestLog("Allowing recipient to respond.");
// Allow the recipient to respond to the donor for the recipientSyncData command that waits on the
// fulfillment of the _dataConsistentPromise. The donor will then send another recipientSyncData
// command that waits on the provided donor timestamp to be majority committed.
fpBeforeDataConsistent.off();

jsTestLog("Reach the point where we are waiting for the tenant oplog applier to catch up.");
fpBeforeApplierFutureCalled.wait();
fpBeforeApplierFutureCalled.off();

jsTestLog("Stepping another node up.");
// Make a new recipient primary step up. This will ask the applier to shutdown.
recipientRst.stepUp(recipientRst.getSecondaries()[0]);

jsTestLog("Release the tenant oplog applier failpoint.");
fpPauseOplogApplier.off();

jsTestLog("Waiting for migration to complete.");
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

tenantMigrationTest.stop();
