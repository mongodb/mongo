/**
 * Tests that we note down the recipient FCV at the beginning of a migration and that a change
 * in that FCV will abort the migration.
 * @tags: [requires_majority_read_concern, requires_fcv_49]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");       // for 'extractUUIDFromObject'
load("jstests/libs/parallelTester.js");  // for 'Thread'
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const recipientRst = new ReplSetTest({
    nodes: 2,
    name: jsTestName() + "_recipient",
    nodeOptions: TenantMigrationUtil.makeX509OptionsForTest().recipient
});

recipientRst.startSet();
recipientRst.initiate();
if (!TenantMigrationUtil.isFeatureFlagEnabled(recipientRst.getPrimary())) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    recipientRst.stopSet();
    return;
}

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), recipientRst: recipientRst});
const tenantId = "testTenantId";
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
const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
const migrationThread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
migrationThread.start();
hangAfterSavingFCV.wait();

// Downgrade the FCV for the recipient set.
assert.commandWorked(
    recipientPrimary.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));

// Step up a new node in the recipient set and trigger a failover. The new primary should attempt to
// resume cloning, but fail upon re-checking the FCV.
const newRecipientPrimary = recipientRst.getSecondaries()[0];
recipientRst.awaitLastOpCommitted();
assert.commandWorked(newRecipientPrimary.adminCommand({replSetStepUp: 1}));
hangAfterSavingFCV.off();
recipientRst.getPrimary();

// The migration will not be able to continue in the downgraded version.
const data = migrationThread.returnData();
assert.commandWorked(data);
assert.eq(data.state, TenantMigrationTest.State.kAborted);
checkLog.containsJson(newRecipientPrimary, 5356200);  // Change-of-FCV detection message.

tenantMigrationTest.stop();
recipientRst.stopSet();
})();
