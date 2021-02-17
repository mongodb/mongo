/*
 * Tests that the recipient will retry a migration on donor sync source failure in the following
 * scenarios:
 * - donor shuts down when the recipient oplog fetcher is created but cloning has yet to start
 * - donor shuts down in the middle of the cloning phase
 * - donor shuts down after cloning is finished but the recipient has yet to declare that the data
 *   is consistent
 *
 * @tags: [requires_majority_read_concern, requires_fcv_49, incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load('jstests/replsets/rslib.js');

function runTest(failPoint) {
    const recipientRst = new ReplSetTest({
        nodes: 2,
        name: jsTestName() + "_recipient",
        // Use a batch size of 2 so that collection cloner requires more than a single batch to
        // complete.
        nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().recipient,
                                   {setParameter: {collectionClonerBatchSize: 2}})
    });
    recipientRst.startSet();
    recipientRst.initiateWithHighElectionTimeout();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), recipientRst, sharedOptions: {nodes: 3}});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        recipientRst.stopSet();
        return false;
    }
    jsTestLog("Running test with failpoint: " + failPoint);
    const tenantId = "testTenantId";
    const tenantDB = tenantMigrationTest.tenantDB(tenantId, "DB");
    const collName = "testColl";

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorSecondary = donorRst.getSecondary();

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const recipientDb = recipientPrimary.getDB(tenantDB);
    let recipientColl = recipientDb.getCollection(collName);

    tenantMigrationTest.insertDonorDB(tenantDB, collName);

    let waitInFailPoint;
    if (failPoint === 'tenantMigrationHangCollectionClonerAfterHandlingBatchResponse') {
        waitInFailPoint =
            configureFailPoint(recipientPrimary, failPoint, {nss: recipientColl.getFullName()});
    } else {
        waitInFailPoint = configureFailPoint(recipientPrimary, failPoint, {action: "hang"});
    }

    const migrationUuid = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationUuid),
        tenantId,
        readPreference: {mode: 'primary'}
    };

    jsTestLog("Starting the tenant migration to wait in failpoint: " + failPoint);
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    waitInFailPoint.wait();
    let res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
    let currOp = res.inprog[0];
    // We should start the migration syncing from the primary.
    assert.eq(donorPrimary.host,
              currOp.donorSyncSource,
              `the recipient should start with 'donorPrimary' as the sync source`);
    let configRecipientNs = recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS);
    let recipientDoc = configRecipientNs.find({"_id": migrationUuid}).toArray();
    assert.eq(recipientDoc[0].state, "started", recipientDoc[0]);
    assert.eq(recipientDoc[0].numRestartsDueToDonorConnectionFailure, 0, recipientDoc[0]);

    jsTestLog("Stopping the donor primary");
    donorRst.stop(donorPrimary);
    waitInFailPoint.off();
    assert.soon(() => {
        // We expect that the recipient is retrying the migration as the donor has shutdown. We will
        // fail trying to find a sync source until a new donor primary is discovered as we will
        // honor the original read preference.
        let recipientDoc = configRecipientNs.find({"_id": migrationUuid}).toArray();
        jsTestLog("recipientDoc:" + tojson(recipientDoc));
        return recipientDoc[0].numRestartsDueToDonorConnectionFailure == 1;
    });

    let hangOnRetry = configureFailPoint(recipientPrimary,
                                         'fpAfterStartingOplogFetcherMigrationRecipientInstance',
                                         {action: "hang"});
    // Step up a new donor primary.
    assert.commandWorked(donorSecondary.adminCommand({replSetStepUp: 1}));
    hangOnRetry.wait();
    res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
    currOp = res.inprog[0];
    // The recipient should resume the migration against the new donor primary.
    assert.eq(donorSecondary.host, currOp.donorSyncSource, currOp);
    hangOnRetry.off();

    assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    // Remove 'donorPrimary' so that the test can complete properly.
    donorRst.remove(donorPrimary);
    recipientRst.stopSet();
    tenantMigrationTest.stop();
    return true;
}

// Test case where donor is shutdown after the recipient has started the oplog fetcher but not the
// cloner.
let testEnabled = runTest('fpAfterStartingOplogFetcherMigrationRecipientInstance');
if (testEnabled) {
    // Test case where donor is shutdown in the middle of the cloning phase.
    runTest('tenantMigrationHangCollectionClonerAfterHandlingBatchResponse');
    // Test case where donor is shutdown after cloning has finished but before the donor is notified
    // that the recipient is in the consistent state.
    runTest('fpAfterStartingOplogApplierMigrationRecipientInstance');
}
})();
