/**
 * Tests that tenant migration fails with NamespaceExists if the recipient already has tenant's
 * data.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   incompatible_with_shard_merge,
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

const kGarbageCollectionParams = {
    // Set the delay before a donor state doc is garbage collected to be short to speed up
    // the test.
    tenantMigrationGarbageCollectionDelayMS: 3 * 1000,

    // Set the TTL monitor to run at a smaller interval to speed up the test.
    ttlMonitorSleepSecs: 1,
};

const donorRst = new ReplSetTest({
    nodes: 1,
    name: "donor",
    nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().donor,
                               {setParameter: kGarbageCollectionParams})
});

donorRst.startSet();
donorRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    donorRst,
    quickGarbageCollection: true,
    sharedOptions: {setParameter: kGarbageCollectionParams}
});

const kTenantId = "testTenantId";
const kNs = kTenantId + "_testDb.testColl";

assert.commandWorked(tenantMigrationTest.getDonorPrimary().getCollection(kNs).insert({_id: 0}));

jsTest.log("Start a tenant migration and verify that it commits successfully");

let migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};

TenantMigrationTest.assertCommitted(
    tenantMigrationTest.runMigration(migrationOpts, {enableDonorStartMigrationFsync: true}));
tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, kTenantId);

jsTest.log(
    "Retry the migration without dropping tenant database on the recipient and verify that " +
    "the migration aborted with NamespaceExists as the abort reason");

migrationId = UUID();
migrationOpts.migrationIdString = extractUUIDFromObject(migrationId);

TenantMigrationTest.assertAborted(tenantMigrationTest.runMigration(migrationOpts),
                                  ErrorCodes.NamespaceExists);

donorRst.stopSet();
tenantMigrationTest.stop();
})();
