/**
 * Tests that recipient is able to fetch and apply all tenant's donor oplog entries from donor.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   featureFlagShardMerge
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {isShardMergeEnabled} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 3}});
const donorPrimary = tenantMigrationTest.getDonorPrimary();

// Note: including this explicit early return here due to the fact that multiversion
// suites will execute this test without featureFlagShardMerge enabled (despite the
// presence of the featureFlagShardMerge tag above), which means the test will attempt
// to run a multi-tenant migration and fail.
if (!isShardMergeEnabled(donorPrimary.getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping Shard Merge-specific test");
    quit();
}

const kTenant0 = ObjectId().str;
const kTenant1 = ObjectId().str;
const kTenant2 = ObjectId().str;

// Insert some documents before migration start so that this collection gets cloned by file cloner.
const collName = "testColl";
const tenantDB0 = tenantMigrationTest.tenantDB(kTenant0, "DB");
assert.commandWorked(donorPrimary.getDB(tenantDB0)[collName].insert({_id: 0}));

const failpoint = "pauseTenantMigrationBeforeLeavingDataSyncState";
const pauseTenantMigrationBeforeLeavingDataSyncState =
    configureFailPoint(donorPrimary, failpoint, {action: "hang"});

const migrationUuid = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationUuid),
    readPreference: {mode: 'primary'},
    tenantIds: [ObjectId(kTenant0), ObjectId(kTenant1), ObjectId(kTenant2)]
};

jsTestLog(`Starting the tenant migration to wait in failpoint: ${failpoint}`);
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

// Wait for the migration to finish file cloning.
pauseTenantMigrationBeforeLeavingDataSyncState.wait();

// Do some more donor writes and this should get migrated to recipient during tenant migration oplog
// catch-up phase.
//
// Add writes to an existing tenant collection.
assert.commandWorked(donorPrimary.getDB(tenantDB0)[collName].update({_id: 0}, {'$set': {x: 1}}));
assert.commandWorked(donorPrimary.getDB(tenantDB0)[collName].insert({_id: 1}));

// Add new tenant collections.
const tenantDB1 = tenantMigrationTest.tenantDB(kTenant1, "DB");
tenantMigrationTest.insertDonorDB(tenantDB1, collName);

const tenantDB2 = tenantMigrationTest.tenantDB(kTenant2, "DB");
tenantMigrationTest.insertDonorDB(tenantDB2, collName);

// Resume migration.
pauseTenantMigrationBeforeLeavingDataSyncState.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

tenantMigrationTest.getRecipientRst().nodes.forEach(node => {
    for (let dbName of [tenantDB0, tenantDB1, tenantDB2]) {
        jsTestLog(`Checking ${dbName}.${collName} on ${node}`);
        // Use "countDocuments" to check actual docs, "count" to check sizeStorer data.
        assert.eq(donorPrimary.getDB(dbName)[collName].countDocuments({}),
                  node.getDB(dbName)[collName].countDocuments({}),
                  "countDocuments");
        assert.eq(donorPrimary.getDB(dbName)[collName].count(),
                  node.getDB(dbName)[collName].count(),
                  "count");
    }
});

tenantMigrationTest.stop();
