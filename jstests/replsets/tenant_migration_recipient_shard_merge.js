/**
 * Tests recipient behavior for shard merge
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

(() => {
    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 3}});

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    if (!TenantMigrationUtil.isShardMergeEnabled(recipientPrimary.getDB("admin"))) {
        tenantMigrationTest.stop();
        jsTestLog("Skipping Shard Merge-specific test");
        return;
    }

    jsTestLog(
        "Test that recipient state is correctly set to 'learned filenames' after creating the backup cursor");
    const tenantId = "testTenantId";
    const tenantDB = tenantMigrationTest.tenantDB(tenantId, "DB");
    const collName = "testColl";

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorSecondary = donorRst.getSecondary();

    tenantMigrationTest.insertDonorDB(tenantDB, collName);

    const failpoint = "fpAfterRetrievingStartOpTimesMigrationRecipientInstance";
    const waitInFailPoint = configureFailPoint(recipientPrimary, failpoint, {action: "hang"});

    const migrationUuid = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationUuid),
        tenantId,
        readPreference: {mode: 'primary'}
    };

    jsTestLog(`Starting the tenant migration to wait in failpoint: ${failpoint}`);
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    waitInFailPoint.wait();

    const res =
        recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
    assert.eq(res.inprog.length, 1);
    const [currOp] = res.inprog;
    assert.eq(currOp.state, TenantMigrationTest.RecipientStateEnum.kLearnedFilenames, res);
    waitInFailPoint.off();

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    tenantMigrationTest.stop();
})();
})();
