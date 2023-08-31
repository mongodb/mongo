/**
 * Tests that tenant migration and shard merge fails upon observing retryable internal transaction
 * writes.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";

const tenantMigrationTest = new TenantMigrationTest(
    {name: jsTestName(), quickGarbageCollection: true, sharedOptions: {nodes: 1}});

const kMigrationId = UUID();
const kTenantId = ObjectId().str;
const kDbName = makeTenantDB(kTenantId, "testDb");
const kCollName = "testColl";
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(kMigrationId),
    tenantId: kTenantId,
};

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const donorPrimaryColl = donorPrimary.getDB(kDbName)[kCollName];
assert.commandWorked(
    donorPrimaryColl.insert({_id: 0, count: 1}, {"writeConcern": {"w": "majority"}}));

jsTestLog("Testing retryable internal transactions started after migration start.");

const fpBeforeMarkingCloneSuccess =
    configureFailPoint(recipientPrimary, "fpBeforeMarkingCloneSuccess", {action: "hang"});

assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

fpBeforeMarkingCloneSuccess.wait();

// Start a retryable internal transaction write.
assert.commandWorked(donorPrimary.getDB("admin").runCommand({
    testInternalTransactions: 1,
    commandInfos: [
        {
            dbName: kDbName,
            command: {
                findAndModify: kCollName,
                query: {_id: 0},
                update: {$inc: {count: 1}},
                stmtId: NumberInt(0),
            },
        },

    ],
    txnNumber: NumberLong(0),
    lsid: {id: UUID()},
}));

fpBeforeMarkingCloneSuccess.off();

TenantMigrationTest.assertAborted(
    tenantMigrationTest.waitForMigrationToComplete(
        migrationOpts, false /* retryOnRetryableErrors */, true /* forgetMigration */),
    ErrorCodes.RetryableInternalTransactionNotSupported);
tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString);

// Drop the tenant database on recipient before retrying migration.
assert.commandWorked(recipientPrimary.getDB(kDbName).dropDatabase());

jsTestLog("Testing retryable internal transactions completed before migration start.");

TenantMigrationTest.assertAborted(tenantMigrationTest.runMigration(
    migrationOpts, ErrorCodes.RetryableInternalTransactionNotSupported));

tenantMigrationTest.stop();
