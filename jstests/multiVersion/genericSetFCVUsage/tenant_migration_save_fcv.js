/**
 * Tests that we note down the recipient FCV at the beginning of a migration and that a change
 * in that FCV will abort the migration.
 * @tags: [
 *  requires_majority_read_concern,
 *  incompatible_with_windows_tls,
 *  serverless,
 *  ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    isShardMergeEnabled,
    makeX509OptionsForTest,
    runMigrationAsync
} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");       // for 'extractUUIDFromObject'
load("jstests/libs/parallelTester.js");  // for 'Thread'
load("jstests/replsets/rslib.js");       // 'createRstArgs'

function runTest(downgradeFCV) {
    const recipientRst = new ReplSetTest({
        nodes: 2,
        name: jsTestName() + "_recipient",
        nodeOptions: makeX509OptionsForTest().recipient
    });

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), recipientRst: recipientRst});
    const tenantId = ObjectId().str;
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const collName = "testColl";

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    tenantMigrationTest.insertDonorDB(dbName, collName);

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId: tenantId,
    };

    // Configure a failpoint to have the recipient primary hang after taking note of its FCV.
    const recipientDb = recipientPrimary.getDB(dbName);
    const hangAfterSavingFCV = configureFailPoint(
        recipientDb, "fpAfterRecordingRecipientPrimaryStartingFCV", {action: "hang"});

    // Start a migration and wait for recipient to hang at the failpoint.
    const donorRstArgs = createRstArgs(tenantMigrationTest.getDonorRst());
    const migrationThread = new Thread(runMigrationAsync, migrationOpts, donorRstArgs);
    migrationThread.start();
    hangAfterSavingFCV.wait();

    const isRunningMergeProtocol = isShardMergeEnabled(recipientDb);

    // Downgrade the FCV for the recipient set.
    assert.commandWorked(
        recipientPrimary.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    // Step up a new node in the recipient set and trigger a failover. The new primary should
    // attempt to resume cloning, but fail upon re-checking the FCV.
    const newRecipientPrimary = recipientRst.getSecondaries()[0];
    recipientRst.awaitLastOpCommitted();
    assert.commandWorked(newRecipientPrimary.adminCommand({replSetStepUp: 1}));
    hangAfterSavingFCV.off();
    recipientRst.getPrimary();

    // The migration will not be able to continue in the downgraded version.
    TenantMigrationTest.assertAborted(migrationThread.returnData());
    // Change-of-FCV detection message.
    if (isRunningMergeProtocol && MongoRunner.compareBinVersions(downgradeFCV, "5.2") < 0) {
        // FCV is too old for shard merge.
        checkLog.containsJson(newRecipientPrimary, 5949504);
    } else {
        // Can't change FCVs during a migration.
        checkLog.containsJson(newRecipientPrimary, 5356200);
    }

    tenantMigrationTest.stop();
    recipientRst.stopSet();
}

runTest(lastContinuousFCV);
if (lastContinuousFCV != lastLTSFCV) {
    runTest(lastLTSFCV);
}
