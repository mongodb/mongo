/**
 * Tests a full tenant migration using the multi-tenant migration protocol while recreating a
 * time-series collection in the place of a dropped regular collection.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    isShardMergeEnabled,
    kProtocolMultitenantMigrations,
    makeTenantDB
} from "jstests/replsets/libs/tenant_migration_util.js";

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const donorPrimary = tenantMigrationTest.getDonorPrimary();

if (isShardMergeEnabled(donorPrimary.getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping this shard merge incompatible test.");
    quit();
}

const tenantId = ObjectId().str;
const tenantDB = makeTenantDB(tenantId, "DB");

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId,
    protocol: kProtocolMultitenantMigrations,
};

// Enable the fail point to hang before the start of collection cloner but after calculation
// startApplyingOptime
const waitInFailPoint = configureFailPoint(
    recipientPrimary, "fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime", {action: "hang"});

// Create a non-timeseries collection.
assert.commandWorked(donorPrimary.getDB(tenantDB).createCollection("foo"));
tenantMigrationTest.getDonorRst().awaitReplication();

assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
waitInFailPoint.wait();

// Perform an insert and DDL operation on the non-timeseries collection.
assert.commandWorked(
    donorPrimary.getDB(tenantDB).foo.insert({_id: 77777}, {"writeConcern": {"w": "majority"}}));
assert.commandWorked(
    donorPrimary.getDB(tenantDB).runCommand({"collMod": "foo", "validationLevel": "moderate"}));

// Drop the non-timeseries collection
donorPrimary.getDB(tenantDB).foo.drop();

// Recreate the collection as timeseries.
const timeseriesCollOption = {
    timeseries: {timeField: "time", metaField: "meta"}
};
assert.commandWorked(donorPrimary.getDB(tenantDB).createCollection("foo", timeseriesCollOption));
tenantMigrationTest.getDonorRst().awaitReplication();

waitInFailPoint.off();
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

tenantMigrationTest.stop();
