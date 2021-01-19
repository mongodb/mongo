/**
 * Starts a tenant migration that aborts, and then issues a donorForgetMigration command. Finally,
 * starts a second tenant migration with the same tenantId as the aborted migration, and expects
 * this second migration to go through.
 *
 * @tags: [requires_fcv_49, requires_majority_read_concern, incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const donorPrimary = tenantMigrationTest.getDonorPrimary();

const tenantId = "testTenantId";

const migrationId1 = extractUUIDFromObject(UUID());
const migrationId2 = extractUUIDFromObject(UUID());

// Start a migration with the "abortTenantMigrationBeforeLeavingBlockingState" failPoint enabled.
// The migration will abort as a result, and a status of "kAborted" should be returned.
jsTestLog("Starting a migration that is expected to abort. migrationId: " + migrationId1 +
          ", tenantId: " + tenantId);
const abortFp = configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
const abortRes = assert.commandWorked(
    tenantMigrationTest.runMigration({migrationIdString: migrationId1, tenantId},
                                     false /* retryOnRetryableErrors */,
                                     false /* automaticForgetMigration */));
assert.eq(abortRes.state, TenantMigrationTest.State.kAborted);
abortFp.off();

// Forget the aborted migration.
jsTestLog("Forgetting aborted migration with migrationId: " + migrationId1);
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationId1));

// Try running a new migration with the same tenantId. It should succeed, since the previous
// migration with the same tenantId was aborted.
jsTestLog("Attempting to run a new migration with the same tenantId. New migrationId: " +
          migrationId2 + ", tenantId: " + tenantId);
const commitRes = assert.commandWorked(
    tenantMigrationTest.runMigration({migrationIdString: migrationId2, tenantId}));
assert.eq(commitRes.state, TenantMigrationTest.State.kCommitted);

tenantMigrationTest.stop();
})();
