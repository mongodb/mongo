/**
 * Tests that the recipient only rejects with writes between when cloning is done and when it
 * receives and reaches the returnAfterReachingTimestamp (blockTimestamp) since no read is allowed
 * in that time window.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls]
 */

(function() {
'use strict';

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

const donorRst = tenantMigrationTest.getDonorRst();
const donorPrimary = donorRst.getPrimary();
const recipientRst = tenantMigrationTest.getRecipientRst();
const recipientPrimary = recipientRst.getPrimary();

const kTenantId = "testTenantId";

(() => {
    jsTest.log("Test writes during and after a migration that commits");

    const tenantId = kTenantId + "Commit";
    const ns = tenantId + "_testDb.testColl";
    const tenantCollOnRecipient = recipientPrimary.getCollection(ns);

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    let startOplogFetcherFp =
        configureFailPoint(recipientPrimary,
                           "fpAfterStartingOplogFetcherMigrationRecipientInstance",
                           {action: "hang"});
    let clonerDoneFp =
        configureFailPoint(recipientPrimary, "fpAfterCollectionClonerDone", {action: "hang"});
    let waitForRejectReadsBeforeTsFp = configureFailPoint(
        recipientPrimary, "fpAfterWaitForRejectReadsBeforeTimestamp", {action: "hang"});

    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);
    const runMigrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    runMigrationThread.start();
    startOplogFetcherFp.wait();

    // Write before cloning is done.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));

    startOplogFetcherFp.off();
    clonerDoneFp.wait();

    // Write after cloning is done should fail with SnapshotTooOld since no read is allowed.
    assert.commandFailedWithCode(tenantCollOnRecipient.remove({_id: 1}), ErrorCodes.SnapshotTooOld);

    clonerDoneFp.off();
    waitForRejectReadsBeforeTsFp.wait();

    // Write after the recipient applied data past the blockTimestamp.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));

    waitForRejectReadsBeforeTsFp.off();
    const stateRes = assert.commandWorked(runMigrationThread.returnData());
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kCommitted);

    // Write after the migration committed.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    // Write after the migration is forgotten.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));
})();

(() => {
    jsTest.log("Test writes after a migration aborted before the recipient receives the " +
               "returnAfterReachingTimestamp");

    const tenantId = kTenantId + "AbortBeforeReturnAfterReachingTs";
    const ns = tenantId + "_testDb.testColl";
    const tenantCollOnRecipient = recipientPrimary.getCollection(ns);

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    // Force the recipient to abort the migration right before it responds to the first
    // recipientSyncData (i.e. before it receives returnAfterReachingTimestamp in the second
    // recipientSyncData).
    let abortFp = configureFailPoint(recipientPrimary,
                                     "fpBeforeFulfillingDataConsistentPromise",
                                     {action: "stop", stopErrorCode: ErrorCodes.InternalError});
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kAborted);
    abortFp.off();

    // Write after the migration aborted.
    assert.commandFailedWithCode(tenantCollOnRecipient.remove({_id: 1}), ErrorCodes.SnapshotTooOld);

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    // Write after the migration is forgotten.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));
})();

(() => {
    jsTest.log("Test writes after the migration aborted after the recipient applied data past" +
               " the returnAfterReachingTimestamp");

    const tenantId = kTenantId + "AbortAfterReturnAfterReachingTs";
    const ns = tenantId + "_testDb.testColl";
    const tenantCollOnRecipient = recipientPrimary.getCollection(ns);

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: kTenantId + "AbortAfterReturnAfterReachingTs",
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    // Force the donor to abort the migration right after the recipient responds to the second
    // recipientSyncData (i.e. after it has reached the returnAfterReachingTimestamp).
    let abortFp =
        configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kAborted);
    abortFp.off();

    // Write after the migration aborted.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    // Write after the migration is forgotten.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));
})();

tenantMigrationTest.stop();
})();