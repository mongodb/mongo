/**
 * Test the waiting logic of $backupCursorExtend. Given a timestamp T, when
 * $backupCursorExtend returns, oplog with T should be majority committed and
 * persisent on the disk of that node.
 *
 * @tags: [
 *   requires_journaling,
 *   requires_persistence,
 *   requires_sharding,
 *   requires_wiredtiger,
 * ]
 */
(function() {
"use strict";
load("jstests/replsets/rslib.js");  // For reconfig, isConfigCommitted and
                                    // safeReconfigShouldFail.
load("jstests/libs/backup_utils.js");
load("jstests/libs/write_concern_util.js");

const DEBUG = false;
const dbName = "test";
const collName = "coll";
const restorePath = MongoRunner.dataPath + "forRestore/";
const numDocs = 2;

let addNodeConfig = function(rst, nodeId, conn, arbiter) {
    const config = rst.getReplSetConfigFromNode();
    if (arbiter) {
        config.members.push({_id: nodeId, host: conn.host, arbiterOnly: true});
    } else {
        config.members.push({_id: nodeId, host: conn.host});
    }

    return config;
};

let removeNodeConfig = function(rst, conn) {
    const config = rst.getReplSetConfigFromNode();
    for (var i = 0; i < config.members.length; i++) {
        if (config.members[i].host == conn.host) {
            config.members.splice(i, 1);
            break;
        }
    }

    return config;
};

function testReconfig(rst, config, shouldSucceed, errCode, errMsg) {
    if (shouldSucceed) {
        reconfig(rst, config);
        assert.soon(() => isConfigCommitted(rst.getPrimary()));
        rst.waitForConfigReplication(rst.getPrimary());
        rst.awaitReplication();
        // rst.await
    } else {
        safeReconfigShouldFail(rst, config, false /* force */, errCode, errMsg);

        // A force reconfig should also fail.
        safeReconfigShouldFail(rst, config, true /* force */, errCode, errMsg);
    }
}

function insertDoc(db, collName, doc) {
    let res = assert.commandWorked(db.runCommand({insert: collName, documents: [doc]}));
    assert(res.hasOwnProperty("operationTime"), tojson(res));
    return res.operationTime;
}

/*
 * Assert that lagged secondary will block when Timestamp T has not been majority committed yet.
 */
function assertLaggedSecondaryGetBlocked() {
    resetDbpath(restorePath);
    let rst = new ReplSetTest({name: "test", nodes: 1});
    rst.startSet();
    rst.initiateWithHighElectionTimeout();
    const primaryDB = rst.getPrimary().getDB(dbName);

    print("Ahoo0 ==> Insert Docs to Primary");
    for (let i = 0; i < 1000; i++) {
        insertDoc(primaryDB, collName, {k: i});
    }

    print("Ahoo0 ==> AddSecondary");
    testReconfig(rst,
                 addNodeConfig(rst, 1 /* nodeId */, rst.add() /* conn */, false /* arbiter */),
                 true /* shouldSucceed */);
    rst.stopSet();
    return;

    let cursor = openBackupCursor(rst.getSecondary());
    // let firstBatch = cursor.next();
    let firstBatch = undefined;
    while(cursor.hasNext()) {
        let batch = cursor.next();
        print("Ahoo1 ==> ", tojson(batch));
        if (!firstBatch) {
            firstBatch = batch;
        }
    }

    print("Ahoo2 --> first batch: ", tojson(firstBatch));
    let checkpointTimestamp = firstBatch.metadata["checkpointTimestamp"];
    const backupId = firstBatch.metadata.backupId;
    print("ahoo2 -> ", tojson(checkpointTimestamp), " "+ backupId);

    jsTestLog("Start writes on primary");
    let clusterTime;
    for (let i = 0; i < numDocs - 1; i++) {
        clusterTime = insertDoc(primaryDB, collName, {a: i});
    }

    print("Ahoo3 ==> clusterTime: ", tojson(clusterTime));
    let extendCursor = extendBackupCursor(rst.getSecondary(), backupId, clusterTime);
    while(extendCursor.hasNext()) {
        let batch = extendCursor.next();
        print("Ahoo3 ==> ", tojson(batch));
    }

    jsTestLog("Start writes on primary");
    for (let i = 0; i < numDocs - 1; i++) {
        clusterTime = insertDoc(primaryDB, collName, {b: i});
    }

    print("Ahoo4 ==> clusterTime: ", tojson(clusterTime));
    extendCursor = extendBackupCursor(rst.getSecondary(), backupId, clusterTime);
    while(extendCursor.hasNext()) {
        let batch = extendCursor.next();
        print("Ahoo4 ==> ", tojson(batch));
    }

    jsTestLog("Start writes on primary");
    for (let i = 0; i < numDocs - 1; i++) {
        clusterTime = insertDoc(primaryDB, collName, {c: i});
    }

    print("Ahoo5 ==> clusterTime: ", tojson(clusterTime));
    extendCursor = extendBackupCursor(rst.getSecondary(), backupId, clusterTime);
    while(extendCursor.hasNext()) {
        let batch = extendCursor.next();
        print("Ahoo5 ==> ", tojson(batch));
    }

    cursor.close();

    cursor = openBackupCursor(rst.getSecondary());
    // let firstBatch = cursor.next();
    while(cursor.hasNext()) {
        let batch = cursor.next();
        print("Ahoo6 ==> ", tojson(batch));
    }

    cursor.close();
    rst.stopSet();
}

assertLaggedSecondaryGetBlocked();
})();
