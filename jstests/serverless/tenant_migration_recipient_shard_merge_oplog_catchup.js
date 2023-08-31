/**
 * Tests that recipient is able to fetch and apply all tenant's donor oplog entries from donor.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 *   requires_shard_merge
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 3}});
const donorPrimary = tenantMigrationTest.getDonorPrimary();

const kTenant0 = ObjectId().str;
const kTenant1 = ObjectId().str;
const kTenant2 = ObjectId().str;

// Insert some documents before migration start so that this collection gets cloned by file cloner.
const collName = "testColl";
const tenantDB0 = makeTenantDB(kTenant0, "DB");
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
const tenantDB1 = makeTenantDB(kTenant1, "DB");
tenantMigrationTest.insertDonorDB(tenantDB1, collName);

const tenantDB2 = makeTenantDB(kTenant2, "DB");
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
