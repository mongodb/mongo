/**
 * Tests that the shard merge protocol cannot start a migration with readPreference other than
 * primary.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   featureFlagShardMerge,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), enableRecipientTesting: false});

const donorPrimary = tenantMigrationTest.getDonorPrimary();

if (!TenantMigrationUtil.isShardMergeEnabled(donorPrimary.getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping Shard Merge-specific test");
    return;
}

const failingMigrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    readPreference: {mode: "secondary"}
};
assert.commandFailedWithCode(tenantMigrationTest.startMigration(failingMigrationOpts),
                             ErrorCodes.FailedToSatisfyReadPreference);

const succeessfulMigrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    readPreference: {mode: "primary"}
};
assert.commandWorked(tenantMigrationTest.startMigration(succeessfulMigrationOpts));

tenantMigrationTest.stop();
})();
