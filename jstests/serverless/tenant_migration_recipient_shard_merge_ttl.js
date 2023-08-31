/**
 * Tests that TTL indexes on the donor are migrated to the recipient and cleanup
 * happens as expected for shard merge.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 *   requires_shard_merge,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), quickGarbageCollection: true});

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const tenantId = ObjectId().str;
const tenantDB = makeTenantDB(tenantId, "DB");
const collName = "testColl";

const donorPrimary = tenantMigrationTest.getDonorPrimary();

const expireAfterSeconds = 1;
donorPrimary.getDB(tenantDB)[collName].insertOne({name: "deleteMe", lastModifiedDate: new Date()});
donorPrimary.getDB(tenantDB)[collName].createIndex({"lastModifiedDate": 1}, {expireAfterSeconds});

let hangTTLMonitorBetweenPasses =
    configureFailPoint(recipientPrimary, "hangTTLMonitorBetweenPasses");

// Pause before TTL on the donor to prevent test documents from being cleaned up before migration.
const waitForTTLPassOnDonor = configureFailPoint(donorPrimary, "hangTTLMonitorBetweenPasses");

const migrationUuid = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationUuid),
    readPreference: {mode: 'primary'},
    tenantIds: [ObjectId(tenantId)]
};

assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

// Wait for a TTL pass to start on the Recipient and then block before continuing.
hangTTLMonitorBetweenPasses.wait();

// Wait for TTL expiry.
sleep(expireAfterSeconds * 1000);

// Unblock the TTL pass on the recipient to let it clean up.
hangTTLMonitorBetweenPasses.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts);

// Wait for another full TTL pass after the migration completes to ensure we have given the document
// a chance to be deleted.
hangTTLMonitorBetweenPasses =
    configureFailPoint(recipientPrimary, "hangTTLMonitorBetweenPasses", {}, {skip: 1});
hangTTLMonitorBetweenPasses.wait();

const documentCount = recipientPrimary.getDB(tenantDB)[collName].countDocuments({name: "deleteMe"});
assert.eq(documentCount, 0);

hangTTLMonitorBetweenPasses.off();

tenantMigrationTest.stop();
