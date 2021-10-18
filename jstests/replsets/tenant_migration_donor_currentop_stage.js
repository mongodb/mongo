/**
 * Tests the human readable "donorStage" currentOp field at various times
 * during a migration.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   multiversion_incompatible,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

// Main test runner.
function runTest(logs, failPoints, descriptions, forgetMigrationCutoffIndex) {
    jsTestLog("Setting up test.");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    // Turn on each failpoint ahead of time. We will reach them one-at-a-time.
    let fps = [];
    for (let i = 0; i < failPoints.length; i++) {
        fps.push(configureFailPoint(donorPrimary, failPoints[i]));
    }

    const tenantId = "testTenantId";
    const migrationId = extractUUIDFromObject(UUID());
    const migrationOpts = {
        migrationIdString: migrationId,
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    jsTestLog("Starting migration");
    const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
    const startMigrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    startMigrationThread.start();

    for (let i = 0; i < forgetMigrationCutoffIndex; i++) {
        checkStage(logs[i], fps[i], descriptions[i], donorPrimary);
    }

    jsTestLog("Waiting for migration to complete");
    startMigrationThread.join();
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    jsTestLog("Forgetting the migration");
    const forgetMigrationThread = new Thread(TenantMigrationUtil.forgetMigrationAsync,
                                             migrationOpts.migrationIdString,
                                             donorRstArgs,
                                             true /* retryOnRetryableErrors */);
    forgetMigrationThread.start();

    for (let i = forgetMigrationCutoffIndex; i < descriptions.length; i++) {
        checkStage(logs[i], fps[i], descriptions[i], donorPrimary);
    }

    forgetMigrationThread.join();

    jsTestLog("Shutting down test");
    tenantMigrationTest.stop();
}

// Checks requested stage against expectations.
function checkStage(log, fp, desc, node) {
    jsTestLog(log);

    fp.wait();

    const res =
        assert.commandWorked(node.adminCommand({currentOp: true, desc: "tenant donor migration"}));
    assert.eq(res.inprog.length, 1, () => tojson(res));
    const instance = res.inprog[0];
    assert.eq(instance.donorStage, desc, () => tojson(res));

    fp.off();
}

runTest(
    [
        "[1] Testing state: kUnstarted",
        "[2] Testing state: kEnteringAbortingIndexBuildsState",
        "[3] Testing state: kAbortingIndexBuilds",
        "[4] Testing state: kFetchingClusterTimeKeys",
        "[5] Testing state: kEnteringDataSyncState",
        "[6] Testing state: kWaitingForRecipientConsistency",
        "[7] Testing state: kEnteringBlockingState",
        "[8] Testing state: kWaitingForRecipientBlockTs",
        "[9] Testing state: kEnteringCommittedState",
        "[10] Testing state: kWaitingForDonorForgetMigration",
        /* forgetMigration cutoff */
        "[11] Testing state: kWaitingForRecipientForgetMigration",
        "[12] Testing state: kMarkingMigrationGarbageCollectable",
        "[13] Testing state: kForgotten",
    ],
    [
        "pauseTenantMigrationBeforeEnteringFutureChain",
        "pauseTenantMigrationAfterPersistingInitialDonorStateDoc",
        "pauseTenantMigrationBeforeAbortingIndexBuilds",
        "pauseTenantMigrationDonorBeforeWaitingForKeysToReplicate",
        "pauseTenantMigrationAfterFetchingAndStoringKeys",
        "pauseTenantMigrationBeforeLeavingDataSyncState",
        "pauseTenantMigrationDonorWhileEnteringBlockingState",
        "pauseTenantMigrationBeforeLeavingBlockingState",
        "pauseTenantMigrationBeforeEnteringCommittedState",
        "pauseWhileWaitingForDonorForgetMigration",
        /* forgetMigration cutoff */
        "pauseTenantMigrationBeforeSendingRecipientForgetMigration",
        "pauseTenantMigrationDonorBeforeMarkingStateGarbageCollectable",
        "pauseTenantMigrationBeforeLeavingFutureChain",
    ],
    [
        "Migration not yet started.",
        "Updating its state document to enter 'aborting index builds' state.",
        "Aborting index builds.",
        "Fetching cluster time key documents from recipient.",
        "Updating its state document to enter 'data sync' state.",
        "Waiting for recipient to finish data sync and become consistent.",
        "Updating its state doc to enter 'blocking' state.",
        "Waiting for receipient to reach the block timestamp.",
        "Updating its state document to enter 'committed' state.",
        "Waiting to receive 'donorForgetMigration' command.",
        /* forgetMigration cutoff */
        "Waiting for recipient to forget migration.",
        "Marking migration as garbage-collectable.",
        "Migration has been forgotten.",
    ],
    10 /* forgetMigrationCutoffIndex */
);
}());
