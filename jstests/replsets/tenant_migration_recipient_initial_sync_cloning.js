/**
 * Tests that during tenant migration, a new recipient node's state document and in-memory state is
 * initialized after initial sync, when 1) the node hasn't begun cloning data yet, 2) is cloning
 * data, and 3) is in the tenant oplog application phase.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   incompatible_with_shard_merge,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load('jstests/replsets/rslib.js');  // for waitForNewlyAddedRemovalForNodeToBeCommitted

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

const testDBName = 'testDB';
const testCollName = 'testColl';

// Restarts a node, allows the node to go through initial sync, and then makes sure its state
// matches up with the primary's. Returns the initial sync node.
function restartNodeAndCheckState(tenantId, tenantMigrationTest, checkMtab) {
    // Restart a node and allow it to complete initial sync.
    const recipientRst = tenantMigrationTest.getRecipientRst();
    const originalRecipientPrimary = recipientRst.getPrimary();

    jsTestLog("Restarting a node from the recipient replica set.");
    let initialSyncNode = recipientRst.getSecondaries()[0];
    initialSyncNode =
        recipientRst.restart(initialSyncNode, {startClean: true, skipValidation: true});

    // Allow the new node to finish initial sync.
    waitForNewlyAddedRemovalForNodeToBeCommitted(originalRecipientPrimary,
                                                 recipientRst.getNodeId(initialSyncNode));
    recipientRst.awaitSecondaryNodes();
    recipientRst.awaitReplication();

    jsTestLog("Ensure that the new node's state matches up with the primary's.");
    // Make sure the new node's state makes sense.
    let recipientDocOnPrimary = undefined;
    let recipientDocOnNewNode = undefined;
    assert.soon(
        () => {
            recipientDocOnPrimary =
                originalRecipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS)
                    .findOne({tenantId: tenantId});
            recipientDocOnNewNode =
                initialSyncNode.getCollection(TenantMigrationTest.kConfigRecipientsNS).findOne({
                    tenantId: tenantId
                });

            return recipientDocOnPrimary.state == recipientDocOnNewNode.state;
        },
        `States never matched, primary: ${recipientDocOnPrimary}, on new node: ${
            recipientDocOnNewNode}`);

    if (checkMtab) {
        jsTestLog("Ensuring TenantMigrationAccessBlocker states match.");
        const primaryMtab = tenantMigrationTest.getTenantMigrationAccessBlocker(
            {recipientNode: originalRecipientPrimary, tenantId});
        const newNodeMtab = tenantMigrationTest.getTenantMigrationAccessBlocker(
            {recipientNode: initialSyncNode, tenantId});

        assert.eq(primaryMtab.recipient.state,
                  newNodeMtab.recipient.state,
                  `Mtab didn't match, primary: ${primaryMtab}, on new node: ${newNodeMtab}`);
    }

    return initialSyncNode;
}

// Restarts a node without tenant oplog application. Ensures its state matches up with the
// primary's, and then steps it up.
function restartNodeAndCheckStateWithoutOplogApplication(
    tenantId, tenantMigrationTest, checkMtab, fpOnRecipient) {
    fpOnRecipient.wait();

    const initialSyncNode = restartNodeAndCheckState(tenantId, tenantMigrationTest, checkMtab);

    jsTestLog("Stepping up the new node.");
    // Now step up the new node
    tenantMigrationTest.getRecipientRst().stepUp(initialSyncNode);
    fpOnRecipient.off();
}

// Pauses the recipient before the tenant oplog application phase, and inserts documents on the
// donor that the recipient tenant oplog applier must apply. Then restarts node, allows initial
// sync, and steps the restarted node up.
function restartNodeAndCheckStateDuringOplogApplication(
    tenantId, tenantMigrationTest, checkMtab, fpOnRecipient) {
    fpOnRecipient.wait();

    // Pause the tenant oplog applier before applying a batch.
    const originalRecipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const fpPauseOplogApplierOnBatch =
        configureFailPoint(originalRecipientPrimary, "fpBeforeTenantOplogApplyingBatch");

    // Insert documents into the donor after data cloning but before tenant oplog application, so
    // that the recipient has entries to apply during tenant oplog application.
    tenantMigrationTest.insertDonorDB(
        tenantMigrationTest.tenantDB(tenantId, testDBName),
        testCollName,
        [...Array(30).keys()].map((i) => ({a: i, b: "George Harrison - All Things Must Pass"})));

    // Wait until the oplog applier has started and is trying to apply a batch. Then restart a node.
    fpPauseOplogApplierOnBatch.wait();
    const initialSyncNode = restartNodeAndCheckState(tenantId, tenantMigrationTest, checkMtab);

    jsTestLog("Stepping up the new node.");
    // Now step up the new node
    tenantMigrationTest.getRecipientRst().stepUp(initialSyncNode);
    fpPauseOplogApplierOnBatch.off();
    fpOnRecipient.off();
}

// This function does the following:
// 1. Configures a failpoint on the recipient primary, depending on the 'recipientFailpoint' that is
//    passed into the function.
// 2. Starts a tenant migration.
// 3. Waits for the recipient failpoint to be hit. Restarts a node, to make it go through initial
//    sync.
// 4. Makes sure the restarted node's state is as expected.
// 5. Steps up the restarted node as the recipient primary, lifts the recipient failpoint, and
//    allows the migration to complete.
function runTestCase(tenantId, recipientFailpoint, checkMtab, restartNodeAndCheckStateFunction) {
    const donorRst = new ReplSetTest({
        name: "donorRst",
        nodes: 1,
        nodeOptions: Object.assign(migrationX509Options.donor, {
            setParameter: {
                // Allow non-timestamped reads on donor after migration completes for testing.
                'failpoint.tenantMigrationDonorAllowsNonTimestampedReads':
                    tojson({mode: 'alwaysOn'}),
            }
        })
    });
    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({
        name: jsTestName(),
        donorRst,
        sharedOptions: {setParameter: {tenantApplierBatchSizeOps: 2}}
    });

    const migrationOpts = {migrationIdString: extractUUIDFromObject(UUID()), tenantId: tenantId};
    const dbName = tenantMigrationTest.tenantDB(tenantId, testDBName);
    const originalRecipientPrimary = tenantMigrationTest.getRecipientPrimary();

    const fpOnRecipient =
        configureFailPoint(originalRecipientPrimary, recipientFailpoint, {action: "hang"});
    tenantMigrationTest.insertDonorDB(dbName, testCollName);

    jsTestLog(`Starting a tenant migration with migrationID ${
        migrationOpts.migrationIdString}, and tenantId ${tenantId}`);
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    restartNodeAndCheckStateFunction(tenantId, tenantMigrationTest, checkMtab, fpOnRecipient);

    // Allow the migration to run to completion.
    jsTestLog("Allowing migration to run to completion.");
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    tenantMigrationTest.stop();
    donorRst.stopSet();
}

// These two test cases are for before the mtab is created, and before the oplog applier has been
// started.
runTestCase('tenantId1',
            "fpAfterStartingOplogFetcherMigrationRecipientInstance",
            false /* checkMtab */,
            restartNodeAndCheckStateWithoutOplogApplication);
runTestCase('tenantId2',
            "tenantCollectionClonerHangAfterCreateCollection",
            false /* checkMtab */,
            restartNodeAndCheckStateWithoutOplogApplication);

// Test case to initial sync a node while the recipient is in the oplog application phase.
runTestCase('tenantId3',
            "fpBeforeFulfillingDataConsistentPromise",
            true /* checkMtab */,
            restartNodeAndCheckStateDuringOplogApplication);

// A case after data consistency so that the mtab exists. We do not care about the oplog applier in
// this case.
runTestCase('tenantId4',
            "fpAfterWaitForRejectReadsBeforeTimestamp",
            true /* checkMtab */,
            restartNodeAndCheckStateWithoutOplogApplication);
})();
