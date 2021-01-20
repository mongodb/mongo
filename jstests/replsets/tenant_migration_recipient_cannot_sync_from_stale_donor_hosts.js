/*
 * Tests that the recipient primary cannot sync from a donor host that has a majority OpTime that is
 * earlier than the recipient's stored 'startApplyingDonorOpTime'.
 *
 * @tags: [requires_majority_read_concern, requires_fcv_49]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load('jstests/replsets/rslib.js');

const donorRst = new ReplSetTest({
    name: `${jsTestName()}_donor`,
    nodes: 3,
    settings: {chainingAllowed: false},
    nodeOptions: TenantMigrationUtil.makeX509OptionsForTest().donor,
});
donorRst.startSet();
donorRst.initiateWithHighElectionTimeout();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    donorRst.stopSet();
    return;
}
const tenantId = "testTenantId";
const tenantDB = tenantMigrationTest.tenantDB(tenantId, "DB");
const collName = "testColl";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const delayedSecondary = donorRst.getSecondaries()[0];
const donorSecondary = donorRst.getSecondaries()[1];

const recipientRst = tenantMigrationTest.getRecipientRst();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const newRecipientPrimary = recipientRst.getSecondary();

// Stop replicating on one of the secondaries so that its majority OpTime will be behind the
// recipient's 'startApplyingDonorOpTime'.
stopServerReplication(delayedSecondary);
tenantMigrationTest.insertDonorDB(tenantDB, collName);

const hangRecipientPrimaryAfterCreatingRSM =
    configureFailPoint(recipientPrimary, 'hangAfterCreatingRSM');
const hangRecipientPrimaryAfterCreatingConnections = configureFailPoint(
    recipientPrimary, 'fpAfterStartingOplogFetcherMigrationRecipientInstance', {action: "hang"});

const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId,
    // The recipient primary can only choose secondaries as sync sources.
    readPreference: {mode: 'secondary'},
};

jsTestLog("Starting the tenant migration");
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
hangRecipientPrimaryAfterCreatingRSM.wait();

awaitRSClientHosts(recipientPrimary, donorSecondary, {ok: true, secondary: true});
awaitRSClientHosts(recipientPrimary, delayedSecondary, {ok: true, secondary: true});

// Turn on the 'waitInHello' failpoint. This will cause the delayed secondary to cease sending hello
// responses and the RSM should mark the node as down. This is necessary so that the delayed
// secondary is not chosen as the sync source here, since we want the 'startApplyingDonorOpTime' to
// be set to the most advanced majority OpTime.
jsTestLog(
    "Turning on waitInHello failpoint. Delayed donor secondary should stop sending hello responses.");
const helloFailpoint = configureFailPoint(delayedSecondary, "waitInHello");
awaitRSClientHosts(recipientPrimary, delayedSecondary, {ok: false});

hangRecipientPrimaryAfterCreatingRSM.off();
hangRecipientPrimaryAfterCreatingConnections.wait();

let res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
let currOp = res.inprog[0];
// The migration should not be complete.
assert.eq(currOp.migrationCompleted, false, tojson(res));
assert.eq(currOp.dataSyncCompleted, false, tojson(res));
// The sync source can only be 'donorSecondary'.
assert.eq(donorSecondary.host, currOp.donorSyncSource, tojson(res));

helloFailpoint.off();

const hangNewRecipientPrimaryAfterCreatingRSM =
    configureFailPoint(newRecipientPrimary, 'hangAfterCreatingRSM');
const hangNewRecipientPrimaryAfterCreatingConnections =
    configureFailPoint(newRecipientPrimary,
                       'fpAfterRetrievingStartOpTimesMigrationRecipientInstance',
                       {action: "hang"});

// Step up a new primary so that the tenant migration restarts on the new primary, with the
// 'startApplyingDonorOpTime' field already set in the state doc.
jsTestLog("Stepping up the recipient secondary");
recipientRst.awaitLastOpCommitted();
recipientRst.stepUp(newRecipientPrimary);
assert.eq(newRecipientPrimary, recipientRst.getPrimary());

// Wait for the new primary to see the state of each donor node.
hangNewRecipientPrimaryAfterCreatingRSM.wait();
awaitRSClientHosts(newRecipientPrimary, donorPrimary, {ok: true, ismaster: true});
awaitRSClientHosts(
    newRecipientPrimary, [delayedSecondary, donorSecondary], {ok: true, secondary: true});
hangNewRecipientPrimaryAfterCreatingRSM.off();

jsTestLog("Releasing failpoints");
hangNewRecipientPrimaryAfterCreatingConnections.wait();
hangRecipientPrimaryAfterCreatingConnections.off();

res = newRecipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
currOp = res.inprog[0];
// 'donorSecondary' should always be the chosen sync source, since read preference is 'secondary'
// and 'delayedSecondary' cannot be chosen because it is too stale.
assert.eq(donorSecondary.host,
          currOp.donorSyncSource,
          `the recipient should always choose the non-lagged secondary as sync source`);

hangNewRecipientPrimaryAfterCreatingConnections.off();
restartServerReplication(delayedSecondary);

assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

donorRst.stopSet();
tenantMigrationTest.stop();
})();
