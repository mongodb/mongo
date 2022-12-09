/**
 * Tests that recipient installs access blockers during both the oplog catchup phase and when
 * creating tenants during file import.
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

const tenantId = ObjectId().str;
const otherTenantId = ObjectId().str;
const existingRecipientTenantId = ObjectId().str;

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

// Note: including this explicit early return here due to the fact that multiversion
// suites will execute this test without featureFlagShardMerge enabled (despite the
// presence of the featureFlagShardMerge tag above), which means the test will attempt
// to run a multi-tenant migration and fail.
if (!TenantMigrationUtil.isShardMergeEnabled(
        tenantMigrationTest.getDonorPrimary().getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping Shard Merge-specific test");
    return;
}

function assertRecipientAccessBlockerPresentFor({tenantId, migrationId}) {
    const {nodes} = tenantMigrationTest.getRecipientRst();

    const {recipientAccessBlockers} =
        TenantMigrationUtil.getTenantMigrationAccessBlockers({recipientNodes: nodes, tenantId});

    assert.eq(recipientAccessBlockers.length,
              nodes.length,
              `recipient access blocker count for "${tenantId}"`);

    recipientAccessBlockers.forEach(accessBlocker => {
        assert.eq(
            accessBlocker.migrationId, migrationId, `recipient access blocker for "${tenantId}"`);
    });
}

function assertRecipientAccessBlockerNotPresentFor({tenantId, migrationId}) {
    const {nodes} = tenantMigrationTest.getRecipientRst();

    const {recipientAccessBlockers} =
        TenantMigrationUtil.getTenantMigrationAccessBlockers({recipientNodes: nodes, tenantId});

    assert.eq(
        recipientAccessBlockers.length, 0, `found recipient access blocker for "${tenantId}"`);
}

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

tenantMigrationTest.insertDonorDB(tenantMigrationTest.tenantDB(tenantId, "DB"), "testColl");
tenantMigrationTest.insertDonorDB(tenantMigrationTest.tenantDB(otherTenantId, "DB"), "testColl");
tenantMigrationTest.insertRecipientDB(tenantMigrationTest.tenantDB(existingRecipientTenantId, "DB"),
                                      "testColl");

const failpoint = "fpBeforeMarkingCloneSuccess";
const waitInFailPoint = configureFailPoint(recipientPrimary, failpoint, {action: "hang"});

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantIds: [ObjectId(tenantId), ObjectId(otherTenantId)],
};

assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

waitInFailPoint.wait();

const newTenantId = ObjectId().str;
tenantMigrationTest.insertDonorDB(tenantMigrationTest.tenantDB(newTenantId, "DB"), "testColl");

[tenantId, otherTenantId].forEach(tenantId => {
    assertRecipientAccessBlockerPresentFor({tenantId, migrationId});
});

[newTenantId, existingRecipientTenantId].forEach(tenantId => {
    assertRecipientAccessBlockerNotPresentFor({tenantId, migrationId});
});

waitInFailPoint.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

[tenantId, otherTenantId, newTenantId].forEach(tenantId => {
    assertRecipientAccessBlockerPresentFor({tenantId, migrationId});
});

[existingRecipientTenantId].forEach(tenantId => {
    assertRecipientAccessBlockerNotPresentFor({tenantId, migrationId});
});

tenantMigrationTest.stop();
})();
