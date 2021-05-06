/**
 * Tests that during tenant migration, a new recipient node's state document and in-memory state is
 * initialized after initial sync, when 1) the node hasn't begun cloning data yet, 2) is cloning
 * data.
 *
 * @tags: [requires_fcv_49, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft, incompatible_with_windows_tls, incompatible_with_macos]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load('jstests/replsets/rslib.js');  // for waitForNewlyAddedRemovalForNodeToBeCommitted

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();
const donorRst = new ReplSetTest({
    name: "donorRst",
    nodes: 1,
    nodeOptions: Object.assign(migrationX509Options.donor, {
        setParameter: {
            // Allow non-timestamped reads on donor after migration completes for testing.
            'failpoint.tenantMigrationDonorAllowsNonTimestampedReads': tojson({mode: 'alwaysOn'}),
        }
    })
});
donorRst.startSet();
donorRst.initiate();

if (!TenantMigrationUtil.isFeatureFlagEnabled(donorRst.getPrimary())) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    donorRst.stopSet();
    return;
}

// Restarts a node, allows the node to go through initial sync, and then makes sure its state
// matches up with the primary's.
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
        const primaryMtab =
            tenantMigrationTest.getTenantMigrationAccessBlocker(originalRecipientPrimary, tenantId);
        const newNodeMtab =
            tenantMigrationTest.getTenantMigrationAccessBlocker(initialSyncNode, tenantId);

        assert.eq(primaryMtab.recipient.state,
                  newNodeMtab.recipient.state,
                  `Mtab didn't match, primary: ${primaryMtab}, on new node: ${newNodeMtab}`);
    }

    jsTestLog("Stepping up the new node.");
    // Now step up the new node
    assert.commandWorked(initialSyncNode.adminCommand({"replSetStepUp": 1}));
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
function runTestCase(tenantId, recipientFailpoint, checkMtab) {
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst: donorRst});

    const migrationOpts = {migrationIdString: extractUUIDFromObject(UUID()), tenantId: tenantId};
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const collName = "testColl";
    const originalRecipientPrimary = tenantMigrationTest.getRecipientPrimary();

    const fpOnRecipient =
        configureFailPoint(originalRecipientPrimary, recipientFailpoint, {action: "hang"});
    tenantMigrationTest.insertDonorDB(dbName, collName);

    jsTestLog(`Starting a tenant migration with migrationID ${
        migrationOpts.migrationIdString}, and tenantId ${tenantId}`);
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    fpOnRecipient.wait();
    restartNodeAndCheckState(tenantId, tenantMigrationTest, checkMtab);
    fpOnRecipient.off();

    // Allow the migration to run to completion.
    jsTestLog("Allowing migration to run to completion.");
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    tenantMigrationTest.stop();
}

runTestCase(
    'tenantId1', "fpAfterStartingOplogFetcherMigrationRecipientInstance", false /* checkMtab */);
runTestCase('tenantId2', "fpAfterDataConsistentMigrationRecipientInstance", true /* checkMtab */);

// TODO: SERVER-57399 Add a test case to initial sync a node while the recipient is in the oplog
// application phase.

donorRst.stopSet();
})();