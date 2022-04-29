/**
 * Tests that the tenant migration recipient correctly prefetches synthetic findAndModify oplog
 * entries with timestamp less than the 'startFetchingDonorTimestamp'.
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

load("jstests/libs/retryable_writes_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/libs/uuid_util.js");        // For extractUUIDFromObject().
load("jstests/libs/fail_point_util.js");  // For configureFailPoint().
load("jstests/libs/parallelTester.js");   // For Thread.

if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
    jsTestLog("Retryable writes are not supported, skipping test");
    return;
}

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const kTenantId = "testTenantId";
const kDbName = `${kTenantId}_testDb`;
const kCollName = "testColl";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const tenantCollection = donorPrimary.getDB(kDbName)[kCollName];

jsTestLog("Run retryable findAndModify prior to the migration startFetchingDonorOpTime");

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};

// Hang before we get the 'startFetchingDonorOpTime'.
const fpBeforeRetrievingStartOpTime =
    configureFailPoint(recipientPrimary, "fpAfterComparingRecipientAndDonorFCV", {action: "hang"});

jsTestLog(`Starting migration: ${tojson(migrationOpts)}`);
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
fpBeforeRetrievingStartOpTime.wait();

// Retryable write with `postImageOpTime`.
const lsid = UUID();
const cmd = {
    findAndModify: kCollName,
    query: {_id: "retryableWrite"},
    update: {$set: {x: 1}},
    new: true,
    upsert: true,
    lsid: {id: lsid},
    txnNumber: NumberLong(2),
    writeConcern: {w: "majority"},
};
assert.commandWorked(tenantCollection.insert({_id: "retryableWrite", count: 0}));
const cmdResponse = assert.commandWorked(donorPrimary.getDB(kDbName).runCommand(cmd));

// Release the previous failpoint to hang after fetching the retryable writes entries before the
// 'startFetchingDonorOpTime'.
const fpAfterPreFetchingRetryableWrites = configureFailPoint(
    recipientPrimary, "fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime", {action: "hang"});
fpBeforeRetrievingStartOpTime.off();
fpAfterPreFetchingRetryableWrites.wait();

const kOplogBufferNS = `repl.migration.oplog_${migrationOpts.migrationIdString}`;
const recipientOplogBuffer = recipientPrimary.getDB("config")[kOplogBufferNS];
jsTestLog({"oplog buffer ns": kOplogBufferNS});
let res = recipientOplogBuffer.find({"entry.o._id": "retryableWrite"}).toArray();
// We have fetched the synthetic no-op post-image oplog entry.
assert.eq(1, res.length, res);
assert.eq("n", res[0].entry.op, res[0]);
const postImageTs = res[0]._id.ts;
// We have yet to fetch the 'findAndModify' oplog entry.
res = recipientOplogBuffer.find({"entry.o2._id": "retryableWrite"}).toArray();
assert.eq(0, res.length, res);

// Resume the migration.
fpAfterPreFetchingRetryableWrites.off();

jsTestLog("Wait for migration to complete");
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
res = recipientOplogBuffer.find({"entry.o._id": "retryableWrite"}).toArray();
assert.eq(1, res.length, res);
// We have now fetched the 'findAndModify' oplog entry.
res = recipientOplogBuffer.find({"entry.o2._id": "retryableWrite"}).toArray();
assert.eq(1, res.length, res);
assert.eq(postImageTs, res[0].entry.postImageOpTime.ts, res[0]);

// Test that retrying the findAndModify on the recipient will give us the same result and postImage.
const retryResponse = assert.commandWorked(recipientPrimary.getDB(kDbName).runCommand(cmd));
// The retry response can contain a different 'clusterTime' and 'operationTime' from the initial
// response.
delete cmdResponse.$clusterTime;
delete retryResponse.$clusterTime;
delete cmdResponse.operationTime;
delete retryResponse.operationTime;
// The retry response contains the "retriedStmtId" field but the initial response does not.
delete retryResponse.retriedStmtId;
assert.eq(0, bsonWoCompare(cmdResponse, retryResponse), retryResponse);

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.stop();
})();
