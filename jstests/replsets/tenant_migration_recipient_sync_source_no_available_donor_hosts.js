/*
 * Tests that the migration cannot complete when at least one donor host has a stale majority OpTime
 * and all other hosts are considered unavailable. The recipient primary should retry and continue
 * to wait until a suitable sync source is available on the donor replica set.
 *
 * @tags: [requires_majority_read_concern, requires_fcv_49]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load('jstests/replsets/rslib.js');

const donorRst = new ReplSetTest({
    name: `${jsTestName()}_donor`,
    nodes: 3,
    settings: {chainingAllowed: false},
    nodeOptions:
        Object.assign(TenantMigrationUtil.makeX509OptionsForTest().donor,
                      {setParameter: {tenantMigrationExcludeDonorHostTimeoutMS: 30 * 1000}}),
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

jsTestLog("Stopping the non-lagged secondary");
donorRst.stop(donorSecondary);

// Wait for the new primary to see the state of each donor node. 'donorSecondary' should return
// '{ok: false}' since it has been shut down.
hangNewRecipientPrimaryAfterCreatingRSM.wait();
awaitRSClientHosts(newRecipientPrimary, donorPrimary, {ok: true, ismaster: true});
awaitRSClientHosts(newRecipientPrimary, delayedSecondary, {ok: true, secondary: true});
awaitRSClientHosts(newRecipientPrimary, donorSecondary, {ok: false});

jsTestLog("Releasing failpoints");
hangNewRecipientPrimaryAfterCreatingRSM.off();
hangRecipientPrimaryAfterCreatingConnections.off();

res = newRecipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
currOp = res.inprog[0];
// The migration should not be complete and there should be no sync source stored, since the new
// recipient primary does not have a valid sync source to choose from.
assert.eq(currOp.migrationCompleted, false, tojson(res));
assert.eq(currOp.dataSyncCompleted, false, tojson(res));
assert(!currOp.donorSyncSource, tojson(res));

jsTestLog("Restarting replication on 'delayedSecondary'");
restartServerReplication(delayedSecondary);
// The recipient should eventually be able to connect to the lagged secondary, after the secondary
// has caught up and the exclude timeout has expired.
// TODO (SERVER-54256): After we add retries in sync source selection for the recipient primary,
// uncomment these lines.
// hangNewRecipientPrimaryAfterCreatingConnections.wait();

// res = newRecipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
// currOp = res.inprog[0];
// assert.eq(
//     delayedSecondary.host,
//     currOp.donorSyncSource,
//     `the new recipient primary should only be able to choose 'delayedSecondary' as sync source`);

// hangNewRecipientPrimaryAfterCreatingConnections.off();

assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

// Remove 'donorSecondary' so that the test can complete properly.
donorRst.remove(donorSecondary);
donorRst.stopSet();
tenantMigrationTest.stop();
})();
