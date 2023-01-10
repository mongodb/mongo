/**
 * Tests running 50 concurrent migrations against the same recipient.
 *
 * Incompatible with shard merge, which can't handle concurrent migrations.
 *
 * @tags: [
 *   incompatible_with_amazon_linux,
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

TestData.logComponentVerbosity = {
    accessControl: {verbosity: 3}
};
const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

// Set up tenant data for the 50 migrations.
const tenantIds = [...Array(50).keys()].map((i) => `testTenantId-${i}`);
let migrationOptsArray = [];
tenantIds.forEach((tenantId) => {
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const collName = "testColl";
    tenantMigrationTest.insertDonorDB(dbName, collName, [{_id: 1}]);
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId: tenantId,
    };
    migrationOptsArray.push(migrationOpts);
});

// Hang all migrations during the cloning phase to avoid some finishing before others so that we
// know we can truly support 50 concurrent migrations.
const hangDuringCollectionClone =
    configureFailPoint(recipientPrimary, "tenantCollectionClonerHangAfterCreateCollection");

// Start the 50 migrations.
migrationOptsArray.forEach((migrationOpts) => {
    jsTestLog("Starting migration for tenant: " + migrationOpts.tenantId);
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
});

jsTestLog("Waiting for failpoint to be hit for all 50 migrations");
assert.commandWorked(recipientPrimary.adminCommand({
    waitForFailPoint: "tenantCollectionClonerHangAfterCreateCollection",
    timesEntered: hangDuringCollectionClone.timesEntered + 50,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Release the failpoint and allow all migration to continue.
hangDuringCollectionClone.off();

migrationOptsArray.forEach((migrationOpts) => {
    jsTestLog("Waiting for migration for tenant: " + migrationOpts.tenantId + " to complete");
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    // Forget migrations first before shutting down the test to prevent unnecessary failover
    // retries.
    jsTestLog("Forgetting migration for tenant: " + migrationOpts.tenantId);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
});

tenantMigrationTest.stop();
})();
