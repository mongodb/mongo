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

const tenantId = "tenantId";
const otherTenantId = "otherTenantId";
const existingRecipientTenantId = "existingRecipientTenantId";

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

function assertRecipientAccessBlockerPresentFor({tenantId, migrationId}) {
    const {nodes: recipientNodes} = tenantMigrationTest.getRecipientRst();

    const {recipientAccessBlockers} =
        TenantMigrationUtil.getTenantMigrationAccessBlockers({recipientNodes, tenantId});

    assert.eq(recipientAccessBlockers.length,
              recipientNodes.length,
              `recipient access blocker count for "${tenantId}"`);

    recipientAccessBlockers.forEach(({migrationId: recipientAccessBlockerMigrationId}) => {
        assert.eq(recipientAccessBlockerMigrationId,
                  migrationId,
                  `recipient access blocker for "${tenantId}"`);
    });
}

function assertRecipientAccessBlockerNotPresentFor({tenantId, migrationId}) {
    const {nodes: recipientNodes} = tenantMigrationTest.getRecipientRst();

    const {recipientAccessBlockers} =
        TenantMigrationUtil.getTenantMigrationAccessBlockers({recipientNodes, tenantId});

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
    // TODO (SERVER-63454): Remove tenantId.
    tenantId,
};

assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

waitInFailPoint.wait();

const newTenantId = "newTenantId";
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
