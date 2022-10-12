/**
 * Tests that writes that are blocked during a shard split will still be able to find out
 * split outcome even if the split's in memory state has been garbage collected.
 *
 * Shard splits are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_62
 * ]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/serverless/libs/shard_split_test.js");

const kGarbageCollectionParams = {
    // Set the delay before a donor state doc is garbage collected to be short to speed up the test.
    shardSplitGarbageCollectionDelayMS: 30 * 1000,
    // Set the TTL monitor to run at a smaller interval to speed up the test.
    ttlMonitorSleepSecs: 1,
};

const kCollName = "testColl";
const kTenantDefinedDbName = "0";

function insertDocument(primaryHost, dbName, collName) {
    jsTestLog("Calling the method insertDocument");
    const primary = new Mongo(primaryHost);
    let primaryDB = primary.getDB(dbName);
    let res = primaryDB.runCommand({insert: collName, documents: [{x: 1}]});
    return res;
}

(() => {
    jsTestLog(
        "Testing blocked writes can see shard split outcome for a split that has been committed and garbage collected.");

    const test = new ShardSplitTest({
        recipientTagName: "recipientNode",
        recipientSetName: "recipient",
        nodeOptions: Object.assign({setParameter: kGarbageCollectionParams})
    });
    test.addRecipientNodes();
    const tenantIds = ["migrationOutcome-committed"];
    const operation = test.createSplitOperation(tenantIds);

    const dbName = test.tenantDB(tenantIds[0], kTenantDefinedDbName);
    const primary = test.donor.getPrimary();
    const primaryDB = primary.getDB(dbName);

    const writeFp = configureFailPoint(primaryDB, "hangWriteBeforeWaitingForMigrationDecision");
    const writeThread = new Thread(insertDocument, primary.host, dbName, kCollName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const blockFp = configureFailPoint(primaryDB, "pauseShardSplitAfterBlocking");
    const commitThread = operation.commitAsync();

    blockFp.wait();

    assertMigrationState(primary, operation.migrationId, "blocking");

    writeThread.start();
    writeFp.wait();

    blockFp.off();
    commitThread.join();

    assert.commandWorked(commitThread.returnData());

    test.removeRecipientNodesFromDonor();
    operation.forget();
    test.waitForGarbageCollection(operation.migrationId, tenantIds);

    writeFp.off();
    writeThread.join();

    const writeRes = writeThread.returnData();
    assert.eq(writeRes.ok, 1);
    assert.eq(writeRes.writeErrors[0].code, ErrorCodes.TenantMigrationCommitted);

    test.stop();
})();

(() => {
    jsTestLog(
        "Testing blocked writes can see shard split outcome for a split that has been aborted and garbage collected.");

    const test = new ShardSplitTest({
        recipientTagName: "recipientNode",
        recipientSetName: "recipient",
        nodeOptions: Object.assign({setParameter: kGarbageCollectionParams})
    });
    test.addRecipientNodes();
    const tenantIds = ["migrationOutcome-committed"];
    const operation = test.createSplitOperation(tenantIds);

    const dbName = test.tenantDB(tenantIds[0], kTenantDefinedDbName);
    const primary = test.donor.getPrimary();
    const primaryDB = primary.getDB(dbName);

    const writeFp = configureFailPoint(primaryDB, "hangWriteBeforeWaitingForMigrationDecision");
    const writeThread = new Thread(insertDocument, primary.host, dbName, kCollName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const abortFp = configureFailPoint(primaryDB, "abortShardSplitBeforeLeavingBlockingState");
    const blockFp = configureFailPoint(primaryDB, "pauseShardSplitAfterBlocking");

    const commitThread = operation.commitAsync();
    blockFp.wait();

    writeThread.start();
    writeFp.wait();

    blockFp.off();

    commitThread.join();

    const res = commitThread.returnData();
    assert.commandFailed(res);
    assertMigrationState(primary, operation.migrationId, "aborted");
    abortFp.off();

    test.removeRecipientNodesFromDonor();
    operation.forget();
    test.waitForGarbageCollection(operation.migrationId, tenantIds);

    writeFp.off();
    writeThread.join();

    const writeRes = writeThread.returnData();
    assert.eq(writeRes.ok, 1);
    assert.eq(writeRes.writeErrors[0].code, ErrorCodes.TenantMigrationAborted);

    test.stop();
})();
})();
