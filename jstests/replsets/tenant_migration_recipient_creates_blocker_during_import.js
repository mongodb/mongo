/**
 * Tests that recipient installs access blockers when creating tenant during file import.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   featureFlagShardMerge,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantId = "tenantId";
const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

if (!TenantMigrationUtil.isShardMergeEnabled(recipientPrimary.getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping Shard Merge-specific test");
    return;
}

const tenantDB = tenantMigrationTest.tenantDB(tenantId, "DB");
const collName = "testColl";
tenantMigrationTest.insertDonorDB(tenantDB, collName);
// Ensure our new collections appear in the backup cursor's checkpoint.
assert.commandWorked(donorPrimary.adminCommand({fsync: 1}));

// TODO (SERVER-63454): This tenantId is still temporarily needed. But we're testing that the
// recipient installs a blocker for "tenantId", which isn't passed to donorStartMigration.
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId: "nonExistentTenantId"
};

TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));
// TenantFileImporterService logs "Create recipient access blocker".
checkLog.containsJson(recipientPrimary.getDB("admin"), 6114100, {tenantId: tenantId});
tenantMigrationTest.stop();
})();
