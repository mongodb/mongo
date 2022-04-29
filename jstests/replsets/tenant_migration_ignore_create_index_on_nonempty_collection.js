/**
 * Tests that recipient ignores createIndex on non-empty collections during oplog application phase.
 * If the recipient sees a createIndex oplog entry and the collection is no longer empty, the index
 * is guaranteed to be dropped after because we block explicit index builds on the donor for the
 * duration of the tenant migration.
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

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/fail_point_util.js");  // For configureFailPoint().

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const kTenantId = "testTenantId";
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const kCollName = "testColl";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const hangAfterRetrievingStartOpTime = configureFailPoint(
    recipientPrimary, 'fpAfterRetrievingStartOpTimesMigrationRecipientInstance', {action: "hang"});

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
};

assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

hangAfterRetrievingStartOpTime.wait();

const donorColl = donorPrimary.getDB(kDbName)[kCollName];
// Create and drop a 2dsphere index on x.
assert.commandWorked(donorColl.createIndex({x: "2dsphere"}));
assert.commandWorked(donorColl.dropIndex({x: "2dsphere"}));

// Insert a document with an x field that would fail to generate a 2dsphere key. If the recipient
// were to reapply the createIndex, the migration would fail.
assert.commandWorked(donorColl.insert({x: 1}, {writeConcern: {w: "majority"}}));

hangAfterRetrievingStartOpTime.off();

// Test that the recipient ignores the createIndex and the migration should succeed.
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

tenantMigrationTest.stop();
})();
