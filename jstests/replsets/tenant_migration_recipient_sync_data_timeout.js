/**
 * Tests that tenant migration does not fail if the recipientSyncData takes a long time to return.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

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

TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
})();
