/**
 * Tests that shard split donor reliably unblocks blocked reads and writes when the split
 * completes or is interrupted when the state doc collection is dropped.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_52,
 *   featureFlagShardSplit
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/serverless/libs/basic_serverless_test.js");

function startReadThread(node, dbName, collName, afterClusterTime) {
    let readThread = new Thread((host, dbName, collName, afterClusterTime) => {
        const node = new Mongo(host);
        node.setSecondaryOk();
        const db = node.getDB(dbName);
        return db.runCommand({
            find: collName,
            readConcern: {afterClusterTime: Timestamp(afterClusterTime.t, afterClusterTime.i)}
        });
    }, node.host, dbName, collName, afterClusterTime);
    readThread.start();
    return readThread;
}

function startWriteThread(node, dbName, collName) {
    let writeThread = new Thread((host, dbName, collName) => {
        const node = new Mongo(host);
        const db = node.getDB(dbName);
        return db.runCommand({insert: collName, documents: [{_id: 1}]});
    }, node.host, dbName, collName);
    writeThread.start();
    return writeThread;
}

const kTenantIdPrefix = "testTenantId";
const kDbName = "testDb";
const kCollName = "testColl";

(() => {
    jsTest.log(
        "Test that a lagged donor secondary correctly unblocks blocked reads after the split aborts");

    const test = new BasicServerlessTest({
        recipientSetName: "recipientSet",
        recipientTagName: "recipientTag",
        quickGarbageCollection: true
    });
    test.addRecipientNodes();

    const donorPrimary = test.donor.getPrimary();
    const donorsColl = donorPrimary.getCollection(BasicServerlessTest.kConfigSplitDonorsNS);
    const tenantId = kTenantIdPrefix + "LaggedSecondaryMigrationAborted";
    const dbName = tenantId + "_" + kDbName;
    assert.commandWorked(
        donorPrimary.getDB(dbName).runCommand({insert: kCollName, documents: [{_id: 0}]}));

    let blockingFp = configureFailPoint(donorPrimary, "pauseShardSplitAfterBlocking");
    let abortFp = configureFailPoint(donorPrimary, "abortShardSplitBeforeLeavingBlockingState");

    const operation = test.createSplitOperation([tenantId]);
    const splitThread = operation.commitAsync();

    blockingFp.wait();
    test.donor.awaitReplication();

    // Run a read command against one of the secondaries, and wait for it to block.
    const laggedSecondary = test.donor.getSecondary();
    const donorDoc = donorsColl.findOne({_id: operation.migrationId});
    assert.neq(null, donorDoc);
    const readThread = startReadThread(laggedSecondary, dbName, kCollName, donorDoc.blockOpTime.ts);
    assert.soon(() => BasicServerlessTest.getNumBlockedReads(laggedSecondary, tenantId) == 1);

    // Disable snapshotting on that secondary, and wait for the split to abort and be garbage
    // collected. That way the secondary is guaranteed to observe the write to set expireAt before
    // learning that the abortOpTime has been majority committed.
    let snapshotFp = configureFailPoint(laggedSecondary, "disableSnapshotting");
    blockingFp.off();

    splitThread.join();
    assert.commandFailed(splitThread.returnData());

    operation.forget();
    test.waitForGarbageCollection(operation.migrationId, operation.tenantIds);

    assert.commandWorked(readThread.returnData());
    abortFp.off();
    snapshotFp.off();

    test.stop();
})();

(() => {
    jsTest.log(
        "Test that a lagged donor secondary correctly unblocks blocked reads after the split commits");
    const test = new BasicServerlessTest({
        recipientSetName: "recipientSet",
        recipientTagName: "recipientTag",
        quickGarbageCollection: true
    });
    test.addRecipientNodes();

    const donorPrimary = test.donor.getPrimary();
    const donorsColl = donorPrimary.getCollection(BasicServerlessTest.kConfigSplitDonorsNS);
    const tenantId = kTenantIdPrefix + "LaggedSecondaryMigrationCommitted";
    const dbName = tenantId + "_" + kDbName;
    assert.commandWorked(
        donorPrimary.getDB(dbName).runCommand({insert: kCollName, documents: [{_id: 0}]}));

    let blockingFp = configureFailPoint(donorPrimary, "pauseShardSplitAfterBlocking");

    const operation = test.createSplitOperation([tenantId]);
    const splitThread = operation.commitAsync();

    blockingFp.wait();
    test.donor.awaitReplication();

    // Run a read command against one of the secondaries, and wait for it to block.
    const laggedSecondary = test.donor.getSecondary();
    const donorDoc = donorsColl.findOne({_id: operation.migrationId});
    assert.neq(null, donorDoc);
    const readThread = startReadThread(laggedSecondary, dbName, kCollName, donorDoc.blockOpTime.ts);
    assert.soon(() => BasicServerlessTest.getNumBlockedReads(laggedSecondary, tenantId) == 1);

    // Disable snapshotting on that secondary, and wait for the split to commit and be garbage
    // collected. That way the secondary is guaranteed to observe the write to set expireAt before
    // learning that the commitOpTime has been majority committed.
    let snapshotFp = configureFailPoint(laggedSecondary, "disableSnapshotting");
    blockingFp.off();

    splitThread.join();
    assert.commandWorked(splitThread.returnData());

    // Remove recipient nodes that have left the set.
    test.removeAndStopRecipientNodes();

    operation.forget();
    test.waitForGarbageCollection(operation.migrationId, operation.tenantIds);

    assert.commandFailedWithCode(readThread.returnData(), ErrorCodes.TenantMigrationCommitted);
    snapshotFp.off();

    test.stop();
})();

(() => {
    jsTest.log(
        "Test that blocked writes and reads are interrupted when the donor's state doc collection is dropped");
    const test = new BasicServerlessTest({
        recipientSetName: "recipientSet",
        recipientTagName: "recipientTag",
        quickGarbageCollection: true
    });
    test.addRecipientNodes();

    const donorPrimary = test.donor.getPrimary();
    const donorsColl = donorPrimary.getCollection(BasicServerlessTest.kConfigSplitDonorsNS);
    const tenantId = kTenantIdPrefix + "DropStateDocCollection";
    const dbName = tenantId + "_" + kDbName;
    assert.commandWorked(
        donorPrimary.getDB(dbName).runCommand({insert: kCollName, documents: [{_id: 0}]}));

    let blockingFp = configureFailPoint(donorPrimary, "pauseShardSplitAfterBlocking");

    const operation = test.createSplitOperation([tenantId]);
    const splitThread = operation.commitAsync();

    blockingFp.wait();

    // Run a read command and a write command against the primary, and wait for them to block.
    const donorDoc = donorsColl.findOne({_id: operation.migrationId});
    assert.neq(null, donorDoc);
    const readThread = startReadThread(donorPrimary, dbName, kCollName, donorDoc.blockOpTime.ts);
    const writeThread = startWriteThread(donorPrimary, dbName, kCollName);
    assert.soon(() => BasicServerlessTest.getNumBlockedReads(donorPrimary, tenantId) == 1);
    assert.soon(() => BasicServerlessTest.getNumBlockedWrites(donorPrimary, tenantId) == 1);

    // Cannot delete the donor state doc since it has not been marked as garbage collectable.
    assert.commandFailedWithCode(donorsColl.remove({}), ErrorCodes.IllegalOperation);

    // Cannot mark the state doc as garbage collectable before the migration commits or aborts.
    assert.commandFailedWithCode(donorsColl.update({recipientSetName: operation.recipientSetName},
                                                   {$set: {expireAt: new Date()}}),
                                 ErrorCodes.BadValue);

    // Can drop the state doc collection but this will not cause all blocked reads and writes to
    // hang.
    assert(donorsColl.drop());
    assert.commandFailedWithCode(readThread.returnData(), ErrorCodes.Interrupted);
    assert.commandFailedWithCode(writeThread.returnData(), ErrorCodes.Interrupted);
    blockingFp.off();

    splitThread.join();

    test.stop();
})();
})();
