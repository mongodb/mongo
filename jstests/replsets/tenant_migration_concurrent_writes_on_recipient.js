/**
 * Tests that the recipient only rejects with writes between when cloning is done and when it
 * receives and reaches the rejectReadsBeforeTimestamp since no read is allowed in that time window.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kTenantId = "testTenantId";

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), quickGarbageCollection: true});

function cleanup(dbName) {
    const donorPrimary = tenantMigrationTest.getDonorRst().getPrimary();
    const donorDB = donorPrimary.getDB(dbName);
    assert.commandWorked(donorDB.dropDatabase());
}

(() => {
    jsTest.log("Test writes during and after a migration that commits");

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientRst().getPrimary();

    const tenantId = `${kTenantId}Commit`;
    const donorDB = `${tenantId}_test`;
    tenantMigrationTest.insertDonorDB(donorDB, "test");
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
    let beforeFetchingTransactionsFp = configureFailPoint(
        recipientPrimary, "fpBeforeFetchingCommittedTransactions", {action: "hang"});
    let waitForRejectReadsBeforeTsFp = configureFailPoint(
        recipientPrimary, "fpAfterWaitForRejectReadsBeforeTimestamp", {action: "hang"});

    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);
    const runMigrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    runMigrationThread.start();
    startOplogFetcherFp.wait();

    if (!TenantMigrationUtil.isShardMergeEnabled(donorPrimary.getDB("adminDB"))) {
        // Write before cloning is done.
        assert.commandFailedWithCode(tenantCollOnRecipient.remove({_id: 1}),
                                     ErrorCodes.SnapshotTooOld);
    }

    startOplogFetcherFp.off();
    beforeFetchingTransactionsFp.wait();

    // Write after cloning is done should fail with SnapshotTooOld since no read is allowed.
    assert.commandFailedWithCode(tenantCollOnRecipient.remove({_id: 1}), ErrorCodes.SnapshotTooOld);

    beforeFetchingTransactionsFp.off();
    waitForRejectReadsBeforeTsFp.wait();

    // Write after the recipient applied data past the rejectReadsBeforeTimestamp.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));

    waitForRejectReadsBeforeTsFp.off();
    TenantMigrationTest.assertCommitted(runMigrationThread.returnData());

    // Write after the migration committed.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    // Write after the migration is forgotten.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));

    tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString,
                                                          migrationOpts.tenantId);
    cleanup(donorDB);
})();

(() => {
    jsTest.log("Test writes after a migration aborted before the recipient receives the " +
               "returnAfterReachingTimestamp");

    const recipientPrimary = tenantMigrationTest.getRecipientRst().getPrimary();

    const tenantId = `${kTenantId}AbortBeforeReturnAfterReachingTs`;
    const donorDB = `${tenantId}_test`;
    tenantMigrationTest.insertDonorDB(donorDB, "test");
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
    TenantMigrationTest.assertAborted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));
    abortFp.off();

    // Write after the migration aborted.
    assert.commandFailedWithCode(tenantCollOnRecipient.remove({_id: 1}), ErrorCodes.SnapshotTooOld);

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    // Write after the migration is forgotten.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));

    tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString,
                                                          migrationOpts.tenantId);
    cleanup(donorDB);
})();

(() => {
    jsTest.log("Test writes after the migration aborted after the recipient finished oplog" +
               " application");

    const donorPrimary = tenantMigrationTest.getDonorRst().getPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientRst().getPrimary();

    const tenantId = `${kTenantId}AbortAfterReturnAfterReachingTs`;
    const donorDB = `${tenantId}_test`;
    tenantMigrationTest.insertDonorDB(donorDB, "test");
    const ns = tenantId + "_testDb.testColl";
    const tenantCollOnRecipient = recipientPrimary.getCollection(ns);

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: kTenantId + "AbortAfterReturnAfterReachingTs",
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    // Force the donor to abort the migration right after the recipient responds to the second
    // recipientSyncData.
    let abortFp =
        configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    TenantMigrationTest.assertAborted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));
    abortFp.off();

    // Write after the migration aborted.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    // Write after the migration is forgotten.
    assert.commandWorked(tenantCollOnRecipient.remove({_id: 1}));

    tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString,
                                                          migrationOpts.tenantId);
    cleanup(donorDB);
})();
tenantMigrationTest.stop();
})();
