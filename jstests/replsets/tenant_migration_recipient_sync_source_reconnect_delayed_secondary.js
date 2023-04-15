/**
 * Tests that a migration will continuously retry sync source selection when there are no available
 * donor hosts. Also checks that a donor host is considered an uneligible sync source when it has a
 * majority OpTime earlier than the recipient's stored 'startApplyingDonorOpTime'.
 *
 * Tests that if the stale donor host advances its majority OpTime to 'startApplyingDonorOpTime'
 * or later, the recipient will successfully choose that donor as sync source and resume the
 * migration.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   # The currentOp output field 'dataSyncCompleted' was renamed to 'migrationCompleted'.
 *   requires_fcv_70,
 *   serverless,
 * ]
 */

import {
    setUpMigrationSyncSourceTest
} from "jstests/replsets/libs/tenant_migration_recipient_sync_source.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";

// After this setUp() call, we should have a migration with 'secondary' read preference. The
// recipient should be continuously retrying sync source selection, unable to choose
// 'delayedSecondary' because it is too stale and 'donorSecondary' because it is down.
const {
    tenantMigrationTest,
    migrationOpts,
    donorSecondary,
    delayedSecondary,
    hangAfterCreatingConnections
} = setUpMigrationSyncSourceTest();

if (!tenantMigrationTest) {
    // Feature flag was not enabled.
    quit();
}

jsTestLog("Restarting replication on 'delayedSecondary'");
restartServerReplication(delayedSecondary);

// The recipient should eventually be able to connect to the lagged secondary, after the secondary
// has caught up and the exclude timeout has expired.
hangAfterCreatingConnections.wait();

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
const currOp = res.inprog[0];
assert.eq(delayedSecondary.host,
          currOp.donorSyncSource,
          `the recipient should only be able to choose 'delayedSecondary' as sync source`);

hangAfterCreatingConnections.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

// Remove 'donorSecondary' so that the test can complete properly.
const donorRst = tenantMigrationTest.getDonorRst();
donorRst.remove(donorSecondary);
donorRst.stopSet();
tenantMigrationTest.stop();
