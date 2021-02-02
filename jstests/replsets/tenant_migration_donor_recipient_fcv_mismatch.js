/**
 * Tests that starting a migration fails if the donor and recipient do not share the same FCV.
 * @tags: [requires_majority_read_concern, requires_fcv_49, incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");       // for 'extractUUIDFromObject'
load("jstests/libs/parallelTester.js");  // for 'Thread'
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const tenantId = "testTenantId";
const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
const collName = "testColl";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

tenantMigrationTest.insertDonorDB(dbName, collName);

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: tenantId,
};

// Configure a failpoint to have the recipient primary hang after taking note of its FCV
// and before comparing it with that of the donor.
const recipientDB = recipientPrimary.getDB(dbName);
const hangAfterSavingFCV = configureFailPoint(
    recipientDB, "fpAfterRecordingRecipientPrimaryStartingFCV", {action: "hang"});

// Start a migration and wait for recipient to hang at the failpoint.
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
hangAfterSavingFCV.wait();

// Downgrade the FCV for the donor set and resume migration.
assert.commandWorked(
    donorPrimary.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));
hangAfterSavingFCV.off();

// Make sure we see the FCV mismatch detection message on the recipient.
checkLog.containsJson(recipientPrimary, 5382300);

// Upgrade again to check on the status of the migration from the donor's point of view.
assert.commandWorked(donorPrimary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
const stateRes =
    assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);

tenantMigrationTest.stop();
})();
