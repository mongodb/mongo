/**
 * Tests that the shard merge recipient correctly prefetches synthetic findAndModify oplog
 * entries with timestamp less than the 'startFetchingDonorTimestamp'. Note, this test is
 * based off of tenant_migration_recipient_fetches_synthetic_find_and_modify_oplog_entries.js
 * but avoids testing implementation details that are not relevant to shard merge.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   featureFlagShardMerge,
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

const lsid1 = UUID();
const lsid2 = UUID();
const cmds = [
    {
        // Retryable write with `postImageOpTime`.
        findAndModify: kCollName,
        query: {_id: "retryableWrite"},
        update: {$set: {x: 1}},
        new: true,
        upsert: true,
        lsid: {id: lsid1},
        txnNumber: NumberLong(2),
        writeConcern: {w: "majority"},
    },
    {
        findAndModify: kCollName,
        query: {_id: "otherRetryableWrite"},
        update: {$inc: {count: 1}},
        new: false,
        upsert: true,
        lsid: {id: lsid2},
        txnNumber: NumberLong(2),
        writeConcern: {w: "majority"},
    }
];
assert.commandWorked(tenantCollection.insert({_id: "retryableWrite", count: 0}));
assert.commandWorked(tenantCollection.insert({_id: "otherRetryableWrite", count: 0}));
const [cmdResponse1, cmdResponse2] =
    cmds.map(cmd => assert.commandWorked(donorPrimary.getDB(kDbName).runCommand(cmd)));

// Release the previous failpoint to hang after fetching the retryable writes entries before the
// 'startFetchingDonorOpTime'.
const fpAfterPreFetchingRetryableWrites = configureFailPoint(
    recipientPrimary, "fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime", {action: "hang"});
fpBeforeRetrievingStartOpTime.off();
fpAfterPreFetchingRetryableWrites.wait();

// Resume the migration.
fpAfterPreFetchingRetryableWrites.off();

jsTestLog("Wait for migration to complete");
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

// Test that retrying the findAndModify commands on the recipient will give us the same results and
// pre or post image.
const [retryResponse1, retryResponse2] =
    cmds.map(cmd => assert.commandWorked(recipientPrimary.getDB(kDbName).runCommand(cmd)));
[[cmdResponse1, retryResponse1], [cmdResponse2, retryResponse2]].forEach(
    ([cmdResponse, retryResponse]) => {
        // The retry response can contain a different 'clusterTime' and 'operationTime' from the
        // initial response.
        delete cmdResponse.$clusterTime;
        delete retryResponse.$clusterTime;
        delete cmdResponse.operationTime;
        delete retryResponse.operationTime;
        // The retry response contains the "retriedStmtId" field but the initial response does not.
        delete retryResponse.retriedStmtId;
    });
assert.eq(0, bsonWoCompare(cmdResponse1, retryResponse1), retryResponse1);
assert.eq(0, bsonWoCompare(cmdResponse2, retryResponse2), retryResponse2);

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.stop();
})();
