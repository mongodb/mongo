/**
 * Tests the human readable "recipientStage" currentOp field at various times
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

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    // Turn on each failpoint ahead of time. We will reach them one-at-a-time.
    let fps = [];
    for (let i = 0; i < failPoints.length; i++) {
        fps.push(configureFailPoint(recipientPrimary, failPoints[i], {action: "hang"}));
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
        checkStage(logs[i], fps[i], descriptions[i], recipientPrimary);
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
        checkStage(logs[i], fps[i], descriptions[i], recipientPrimary);
    }

    forgetMigrationThread.join();

    jsTestLog("Shutting down test");
    tenantMigrationTest.stop();
}

// Checks requested stage against expectations.
function checkStage(log, fp, desc, node) {
    jsTestLog(log);

    fp.wait();

    const res = assert.commandWorked(
        node.adminCommand({currentOp: true, desc: "tenant recipient migration"}));
    assert.eq(res.inprog.length, 1, () => tojson(res));
    const instance = res.inprog[0];
    assert.eq(instance.recipientStage, desc, () => tojson(res));

    fp.off();
}

runTest(
    [
        "[1] Testing state: kUnstarted",
        "[2] Testing state: kInitializing",
        "[3] Testing state: kFetchingClusterTimeKeys",
        "[4] Testing state: kSavingOwnFCV",
        "[5] Testing state: kFetchingDonorFCV",
        "[6] Testing state: kGettingStartOpTimes",
        "[7] Testing state: kCreatingOplogBuffer",
        "[8] Testing state: kStartingOplogFetcher",
        "[9] Testing state: kStartingDataSync",
        "[10] Testing state: kWaitingForClone",
        "[11] Testing state: kFetchingCommittedTransactions",
        "[12] Testing state: kStartingOplogApplier",
        /* forgetMigration cutoff */
        "[13] Testing state: kWaitingForForgetMigrationCmd",
        "[14] Testing state: kMarkingStateDocGarbageCollectable",
        "[15] Testing state: kForgotten",

    ],
    [
        "pauseBeforeRunTenantMigrationRecipientInstance",
        "pauseAtStartOfTenantMigrationRecipientFutureChain",
        "fpBeforeFetchingDonorClusterTimeKeys",
        "fpAfterConnectingTenantMigrationRecipientInstance",
        "fpAfterRecordingRecipientPrimaryStartingFCV",
        "fpAfterComparingRecipientAndDonorFCV",
        "fpAfterRetrievingStartOpTimesMigrationRecipientInstance",
        "fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime",
        "fpAfterStartingOplogFetcherMigrationRecipientInstance",
        "pauseTenantMigrationAfterSettingWaitingForCloneStage",
        "fpAfterCollectionClonerDone",
        "fpAfterFetchingCommittedTransactions",
        /* forgetMigration cutoff */
        "pauseTenantMigrationRecipientBeforeForgetMigration",
        "fpAfterReceivingRecipientForgetMigration",
        "pauseTenantMigrationRecipientBeforeLeavingFutureChain",
    ],
    [
        "Not yet started.",
        "Initializing state document.",
        "Fetching cluster time key documents from donor.",
        "Checking FCV and saving it in the state document.",
        "Fetching donor FCV to compare with own FCV.",
        "Getting start optimes from donor.",
        "Creating oplog buffer collection.",
        "Starting oplog fetcher.",
        "Creating oplog applier and starting data clone.",
        "Waiting for data clone to complete.",
        "Fetching committed transactions before startOpTime",
        "Running oplog applier until data consistency is reached.",
        /* forgetMigration cutoff */
        "Waiting to receive a recipientForgetMigration command.",
        "Marking state doc as garbage collectable.",
        "Migration has been forgotten.",
    ],
    12 /* forgetMigrationCutoffIndex */
);
}());
