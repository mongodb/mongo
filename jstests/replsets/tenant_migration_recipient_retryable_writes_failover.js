/**
 * Tests whether the recipient correctly clears its oplog buffer if the recipient primary
 * fails over while fetching retryable writes oplog entries from the donor.
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

(function() {

"use strict";
load("jstests/libs/uuid_util.js");        // For extractUUIDFromObject().
load("jstests/libs/fail_point_util.js");  // For configureFailPoint().
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 2}});

const kMigrationId = UUID();
const kTenantId = 'testTenantId';
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDb");
const kCollName = "testColl";
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(kMigrationId),
    tenantId: kTenantId,
};

const donorRst = tenantMigrationTest.getDonorRst();
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const rsConn = new Mongo(donorRst.getURL());
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const session = rsConn.startSession({retryWrites: true});
const sessionColl = session.getDatabase(kDbName)[kCollName];

const session2 = rsConn.startSession({retryWrites: true});
const sessionColl2 = session2.getDatabase(kDbName)[kCollName];

jsTestLog("Run retryable writes prior to the migration.");
assert.commandWorked(sessionColl.insert({_id: "retryableWrite1"}));
assert.commandWorked(sessionColl2.insert({_id: "retryableWrite2"}));

jsTestLog("Setting up failpoints.");
// Use `pauseAfterRetrievingRetryableWritesBatch` to hang after inserting the first batch of results
// from the aggregation request into the oplog buffer.
const fpPauseAfterRetrievingRetryableWritesBatch =
    configureFailPoint(recipientPrimary, "pauseAfterRetrievingRetryableWritesBatch");

// Set aggregation request batch size to 1 so that we can failover in between batches.
const fpSetSmallAggregationBatchSize =
    configureFailPoint(recipientPrimary, "fpSetSmallAggregationBatchSize");

jsTestLog("Starting tenant migration with migrationId: " + kMigrationId +
          ", tenantId: " + kTenantId);
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

jsTestLog("Waiting until the recipient primary fetches a batch of retryable writes oplog entries.");
fpSetSmallAggregationBatchSize.wait();
fpPauseAfterRetrievingRetryableWritesBatch.wait();

// Check that the oplog buffer is correctly populated.
const kOplogBufferNS = "repl.migration.oplog_" + migrationOpts.migrationIdString;
let recipientOplogBuffer = recipientPrimary.getDB("config")[kOplogBufferNS];
// We expect to have only retryableWrite1 since the cursor batch size is 1 and we paused after
// inserting the first branch of results from the aggregation request.
let cursor = recipientOplogBuffer.find();
assert.eq(cursor.itcount(), 1, "Incorrect number of oplog entries in buffer: " + cursor.toArray());

// Check that we haven't completed the retryable writes fetching stage yet.
let recipientConfigColl = recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS);
let recipientDoc = recipientConfigColl.find({"_id": kMigrationId}).toArray();
assert.eq(recipientDoc.length, 1);
assert.eq(recipientDoc[0].completedFetchingRetryableWritesBeforeStartOpTime, false);

jsTestLog("Stepping a new primary up.");
const recipientRst = tenantMigrationTest.getRecipientRst();
const recipientSecondary = recipientRst.getSecondary();
// Use `fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime` to hang after populating the oplog
// buffer with retryable writes entries. Set this before stepping up instead of after so that the
// new primary will not be able to pass this stage without the failpoint being set.
const fpAfterFetchingRetryableWritesEntries = configureFailPoint(
    recipientSecondary, "fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime", {action: "hang"});

recipientRst.stepUp(recipientSecondary);

fpPauseAfterRetrievingRetryableWritesBatch.off();
const newRecipientPrimary = recipientRst.getPrimary();

fpAfterFetchingRetryableWritesEntries.wait();
// The new primary should have cleared its oplog buffer and refetched both retryableWrite1 and
// retryableWrite2. Otherwise, we will invariant when trying to add those entries.
recipientOplogBuffer = newRecipientPrimary.getDB("config")[kOplogBufferNS];
cursor = recipientOplogBuffer.find();
assert.eq(cursor.itcount(), 2, "Incorrect number of oplog entries in buffer: " + cursor.toArray());

recipientConfigColl = newRecipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS);
recipientDoc = recipientConfigColl.find({"_id": kMigrationId}).toArray();
assert.eq(recipientDoc.length, 1);
assert.eq(recipientDoc[0].completedFetchingRetryableWritesBeforeStartOpTime, true);

fpAfterFetchingRetryableWritesEntries.off();
fpSetSmallAggregationBatchSize.off();

jsTestLog("Waiting for migration to complete.");
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

tenantMigrationTest.stop();
})();
