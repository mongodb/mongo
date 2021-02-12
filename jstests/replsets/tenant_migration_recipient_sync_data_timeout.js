/**
 * Tests that tenant migration does not fail if the recipientSyncData takes a long time to return.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls]
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

const kTenantId = "testTenantId";

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
configureFailPoint(recipientPrimary, "failCommand", {
    failInternalCommands: true,
    blockConnection: true,
    blockTimeMS: 35 * 1000,
    failCommands: ["recipientSyncData"],
});

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};

const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
})();
