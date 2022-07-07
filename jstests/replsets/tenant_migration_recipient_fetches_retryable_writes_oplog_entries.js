/**
 * Tests that the tenant migration recipient correctly fetches retryable writes oplog entries
 * and adds them to its oplog buffer.
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
load("jstests/libs/uuid_util.js");        // For extractUUIDFromObject().
load("jstests/libs/fail_point_util.js");  // For configureFailPoint().
load("jstests/libs/parallelTester.js");   // For Thread.

const kMaxBatchSize = 1;

function runTest({storeFindAndModifyImagesInSideCollection = false}) {
    const tenantMigrationTest = new TenantMigrationTest({
        name: jsTestName(),
        quickGarbageCollection: true,
        sharedOptions: {
            nodes: 1,
            setParameter: {
                // Decrease internal max batch size so we can still show writes are batched without
                // inserting hundreds of documents.
                internalInsertMaxBatchSize: kMaxBatchSize,
            }
        }
    });

    const kTenantId = "testTenantId";
    const kTenantId2 = "testTenantId2";
    const kDbName = `${kTenantId}_testDb`;
    const kDbName2 = `${kTenantId2}_testDb`;
    const kCollName = "testColl";

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const setParam = {
        setParameter: 1,
        storeFindAndModifyImagesInSideCollection,
    };
    donorPrimary.adminCommand(setParam);
    recipientPrimary.adminCommand(setParam);
    const rsConn = new Mongo(donorRst.getURL());

    const tenantSession = rsConn.startSession({retryWrites: true});
    const tenantCollection = tenantSession.getDatabase(kDbName)[kCollName];

    const tenantSession2 = rsConn.startSession({retryWrites: true});
    const tenantCollection2 = tenantSession2.getDatabase(kDbName)[kCollName];

    const tenantSession3 = rsConn.startSession({retryWrites: true});
    const tenantCollection3 = tenantSession3.getDatabase(kDbName)[kCollName];

    // Create a collection on a database that isn't prefixed with `kTenantId`.
    const secondTenantSession = rsConn.startSession({retryWrites: true});
    const secondTenantCollection = secondTenantSession.getDatabase(kDbName2)[kCollName];

    const isShardMergeEnabled =
        TenantMigrationUtil.isShardMergeEnabled(donorRst.getPrimary().getDB("adminDB"));

    jsTestLog("Run retryable writes prior to the migration");

    // Retryable insert, but not on correct tenant database. This write should not show up in the
    // oplog buffer for the tenant migration protocol. It will however for the shard merge protocol.
    assert.commandWorked(secondTenantCollection.insert({_id: "retryableWrite1"}));

    // The following retryable writes should occur on the correct tenant database, so they should
    // all be retrieved by the pipeline.
    assert.commandWorked(tenantCollection.insert({_id: "retryableWrite2"}));

    // Retryable write with `postImageOpTime`.
    assert.commandWorked(tenantCollection2.insert({_id: "retryableWrite3", count: 0}));
    tenantCollection2.findAndModify(
        {query: {_id: "retryableWrite3"}, update: {$inc: {count: 1}}, new: true});

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };

    jsTestLog("Set up failpoints.");
    // Use `hangDuringBatchInsert` on the donor to hang after the first batch of a bulk insert. The
    // first batch only has one write and its `lastWriteOpTime` should be before
    // `startFetchingDonorOpTime`.
    const writeFp = configureFailPoint(donorPrimary, "hangDuringBatchInsert", {}, {skip: 1});

    const batchInsertWorker = new Thread((host, dbName, collName, numToInsert) => {
        // Insert elements [{_id: bulkRetryableWrite0}, {_id: bulkRetryableWrite1}].
        const docsToInsert =
            [...Array(numToInsert).keys()].map(i => ({_id: `bulkRetryableWrite${i}`}));

        donorConn = new Mongo(host);
        const tenantSession4 = donorConn.startSession({retryWrites: true});
        const tenantCollection4 = tenantSession4.getDatabase(dbName)[collName];

        assert.commandWorked(
            tenantCollection4.insert(docsToInsert, {writeConcern: {w: "majority"}, ordered: true}));
    }, donorPrimary.host, kDbName, kCollName, 2 * kMaxBatchSize);
    batchInsertWorker.start();
    writeFp.wait();

    // Use `fpAfterRetrievingStartOpTimesMigrationRecipientInstance` to hang after obtaining
    // `startFetchingDonorOpTime`.
    const fpAfterRetrievingStartOpTime =
        configureFailPoint(recipientPrimary,
                           "fpAfterRetrievingStartOpTimesMigrationRecipientInstance",
                           {action: "hang"});

    // Use `fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime` to hang after populating the
    // oplog buffer.
    const fpAfterRetrievingRetryableWrites =
        configureFailPoint(recipientPrimary,
                           "fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime",
                           {action: "hang"});

    jsTestLog(`Starting migration: ${tojson(migrationOpts)}`);
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    fpAfterRetrievingStartOpTime.wait();

    // After we've calculated `startFetchingDonorOpTime`, allow the bulk insert to complete its
    // second batch, which only has one write. This write should have a `lastWriteOpTime` after
    // `startFetchingDonorOpTime`.
    // Now, we've created a retryable writes chain where write1TS < startFetchingTS < write2TS. We
    // must fetch the entry that occurred before `startFetchingDonorOpTime` (write1) and discard the
    // one that occurred after (write2).
    writeFp.off();
    batchInsertWorker.join();

    // Do a retryable write on the donor after hitting `fpAfterRetrievingStartOpTime` so that its
    // `lastWriteOpTime` is after `startFetchingDonorOpTime`. The corresponding oplog entries should
    // not be added to the oplog buffer.
    assert.commandWorked(tenantCollection3.insert({_id: "retryableWrite4", count: 0},
                                                  {writeConcern: {w: "majority"}}));

    // Test that when a post image op's `postImageOpTime` is after `startFetchingDonorOpTime`, it
    // gets filtered out.
    tenantCollection3.findAndModify({
        query: {_id: "retryableWrite4"},
        update: {$inc: {count: 1}},
        new: true,
        writeConcern: {w: "majority"}
    });

    // Test that when a pre image op's `preImageOpTime` is after `startFetchingDonorOpTime`, it gets
    // filtered out.
    tenantCollection3.findAndModify(
        {query: {_id: "retryableWrite4"}, remove: true, writeConcern: {w: "majority"}});

    fpAfterRetrievingStartOpTime.off();
    fpAfterRetrievingRetryableWrites.wait();

    const kOplogBufferNS = `repl.migration.oplog_${migrationOpts.migrationIdString}`;
    const recipientOplogBuffer = recipientPrimary.getDB("config")[kOplogBufferNS];
    jsTestLog({"oplog buffer ns": kOplogBufferNS});

    // We expect to see retryableWrite2, retryableWrite3, retryableWrite3's postImage,
    // and bulkRetryableWrite0 (bulk insert batch size is 1).
    assert.eq(1, recipientOplogBuffer.find({"entry.o._id": "retryableWrite2"}).itcount());
    assert.eq(1, recipientOplogBuffer.find({"entry.o._id": "retryableWrite3"}).itcount());
    assert.eq(1, recipientOplogBuffer.find({"entry.o2._id": "retryableWrite3"}).itcount());
    assert.eq(1, recipientOplogBuffer.find({"entry.o._id": "bulkRetryableWrite0"}).itcount());

    // Only for shardMerge we expect to have the other tenantId. Otherwise only for the provided
    // tenantId.
    assert.eq(isShardMergeEnabled ? 1 : 0,
              recipientOplogBuffer.find({"entry.o._id": "retryableWrite1"}).itcount());

    // Ensure the retryable write oplog entries that should not be in `kOplogBufferNS` are in fact
    // not.
    assert.eq(0, recipientOplogBuffer.find({"entry.o._id": "retryableWrite4"}).itcount());
    assert.eq(0, recipientOplogBuffer.find({"entry.o2._id": "retryableWrite4"}).itcount());
    assert.eq(0, recipientOplogBuffer.find({"entry.o._id": "bulkRetryableWrite1"}).itcount());

    fpAfterRetrievingRetryableWrites.off();

    jsTestLog("Wait for migration to complete");
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, kTenantId);
    tenantMigrationTest.stop();
}

runTest({storeFindAndModifyImagesInSideCollection: false});
runTest({storeFindAndModifyImagesInSideCollection: true});
})();
