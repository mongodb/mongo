/**
 * Tests that the recipient will abort a shard merge on donor failure
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   featureFlagShardMerge,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {isShardMergeEnabled} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");

(() => {
    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 3}});

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    if (!isShardMergeEnabled(recipientPrimary.getDB("admin"))) {
        tenantMigrationTest.stop();
        jsTestLog("Skipping Shard Merge-specific test");
        return;
    }

    jsTestLog("Test that a shard merge is aborted in the event of a donor failure");
    const tenantId = ObjectId().str;
    const tenantDB = tenantMigrationTest.tenantDB(tenantId, "DB");
    const collName = "testColl";

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorSecondary = donorRst.getSecondary();

    tenantMigrationTest.insertDonorDB(tenantDB, collName);

    const failpoint = "fpAfterComparingRecipientAndDonorFCV";
    const waitInFailPoint = configureFailPoint(recipientPrimary, failpoint, {action: "hang"});

    const migrationUuid = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationUuid),
        readPreference: {mode: 'primary'},
        tenantIds: [ObjectId(tenantId)],
    };

    jsTestLog(`Starting the tenant migration to wait in failpoint: ${failpoint}`);
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    waitInFailPoint.wait();
    jsTestLog("Stopping the donor primary");
    donorRst.stop(donorPrimary);

    // wait until the completion path has started after the abort
    const hangBeforeTaskCompletion =
        configureFailPoint(recipientPrimary, "hangBeforeTaskCompletion", {action: "hang"});

    waitInFailPoint.off();
    hangBeforeTaskCompletion.wait();

    // step up a secondary so that the migration will complete and the
    // waitForMigrationToComplete call to the donor primary succeeds
    assert.soonNoExcept(() => {
        return assert.commandWorked(donorSecondary.adminCommand({replSetStepUp: 1}));
    });
    hangBeforeTaskCompletion.off();

    TenantMigrationTest.assertAborted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    tenantMigrationTest.stop();
})();
