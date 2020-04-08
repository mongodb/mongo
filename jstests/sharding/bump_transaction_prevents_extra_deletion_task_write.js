/*
 *  Tests that the deletion task is not written if the donor recovers after the decision is
 * recorded.
 */

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const dbName = "test";

// Create 2 shards with 3 replicas each.
let st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

function getNewNs(dbName) {
    if (typeof getNewNs.counter == 'undefined') {
        getNewNs.counter = 0;
    }
    getNewNs.counter++;
    const collName = "ns" + getNewNs.counter;
    return [collName, dbName + "." + collName];
}

function moveChunkParallel(ns, toShard) {
    return startParallelShell(funWithArgs(function(ns, toShard) {
                                  db.adminCommand({moveChunk: ns, find: {x: 50}, to: toShard});
                              }, ns, toShard), st.s.port);
}

function sendRecvChunkStart(conn, ns, id, sessionId, lsid, from, to) {
    let cmd = {
        _recvChunkStart: ns,
        uuid: id,
        lsid: lsid,
        txnNumber: NumberLong(0),
        sessionId: sessionId,
        from: from.host,
        fromShardName: from.shardName,
        toShardName: to.shardName,
        min: {x: 50.0},
        max: {x: MaxKey},
        shardKeyPattern: {x: 1.0},
        resumableRangeDeleterDisabled: false
    };

    return conn.getDB("admin").runCommand(cmd);
}

function sendRecvChunkStatus(conn, ns, sessionId) {
    let cmd = {
        _recvChunkStatus: ns,
        waitForSteadyOrDone: true,
        sessionId: sessionId,
    };

    return conn.getDB("admin").runCommand(cmd);
}

(() => {
    jsTestLog(
        "Test that recovering a migration coordination ensures a delayed _recvChunkStart does not \
         cause the recipient to re-insert a range deletion task");

    const [collName, ns] = getNewNs(dbName);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));

    // Insert documents into both chunks on shard0.
    let testColl = st.s.getDB(dbName).getCollection(collName);
    for (let i = 0; i < 100; i++) {
        testColl.insert({x: i});
    }

    let donorPrimary = st.rs0.getPrimary();
    let failpoint = configureFailPoint(donorPrimary, 'moveChunkHangAtStep3');

    // Move chunk [50, inf) to shard1.
    const awaitShell = moveChunkParallel(ns, st.shard1.shardName);

    failpoint.wait();

    // Get the migration doc from the donor.
    let migrationDocColl = donorPrimary.getDB("config").migrationCoordinators;
    let migrationDoc = migrationDocColl.findOne();
    jsTestLog("migration doc: " + tojson(migrationDoc));

    // Step down current primary.
    assert.commandWorked(donorPrimary.adminCommand({replSetStepDown: 60, force: 1}));

    failpoint.off();
    awaitShell();

    // Recovery should have occurred. Get new primary and verify migration doc is deleted.
    donorPrimary = st.rs0.getPrimary();
    migrationDocColl = donorPrimary.getDB("config").migrationCoordinators;

    assert.soon(() => {
        return migrationDocColl.find().itcount() == 0;
    });

    let recipientPrimary = st.rs1.getPrimary();

    // Wait until the deletion task has been processed on recipient before sending the second
    // recvChunkStart.
    assert.soon(() => {
        return recipientPrimary.getDB("config").rangeDeletions.find().itcount() == 0;
    });

    // Simulate that the recipient received a delayed _recvChunkStart message by sending one
    // directly to the recipient, and ensure that the _recvChunkStart fails with TransactionTooOld
    // without inserting a new range deletion task.Since the business logic of
    //_recvChunkStart is executed asynchronously, use _recvChunkStatus to check the
    // result of the _recvChunkStart.
    assert.commandWorked(sendRecvChunkStart(recipientPrimary,
                                            ns,
                                            migrationDoc._id,
                                            migrationDoc.migrationSessionId,
                                            migrationDoc.lsid,
                                            st.shard0,
                                            st.shard1));

    assert.soon(() => {
        let result = sendRecvChunkStatus(recipientPrimary, ns, migrationDoc.migrationSessionId);
        jsTestLog("recvChunkStatus: " + tojson(result));

        return result.state === "fail" &&
            result.errmsg.startsWith("migrate failed: TransactionTooOld:");
    });

    // Verify deletion task doesn't exist on recipient.
    assert.eq(recipientPrimary.getDB("config").rangeDeletions.find().itcount(), 0);
})();

(() => {
    jsTestLog(
        "Test that completing a migration coordination at the end of moveChunk ensures a delayed \
         _recvChunkStart does not cause the recipient to re - insert a range deletion task");

    const [collName, ns] = getNewNs(dbName);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));

    // Insert documents into both chunks on shard0.
    let testColl = st.s.getDB(dbName).getCollection(collName);
    for (let i = 0; i < 100; i++) {
        testColl.insert({x: i});
    }

    let donorPrimary = st.rs0.getPrimary();
    let failpoint = configureFailPoint(donorPrimary, 'moveChunkHangAtStep3');

    // Move chunk [50, inf) to shard1.
    const awaitShell = moveChunkParallel(ns, st.shard1.shardName);

    failpoint.wait();

    // Get the migration doc from the donor.
    let migrationDocColl = donorPrimary.getDB("config").migrationCoordinators;
    let migrationDoc = migrationDocColl.findOne();
    jsTestLog("migration doc: " + tojson(migrationDoc));

    failpoint.off();
    awaitShell();

    // Simulate that the recipient received a delayed _recvChunkStart message by sending one
    // directly to the recipient, and ensure that the _recvChunkStart fails with TransactionTooOld
    // without inserting a new range deletion task.Since the business logic of
    //_recvChunkStart is executed asynchronously, use _recvChunkStatus to check the
    // result of the _recvChunkStart.
    let recipientPrimary = st.rs1.getPrimary();
    assert.commandWorked(sendRecvChunkStart(recipientPrimary,
                                            ns,
                                            migrationDoc._id,
                                            migrationDoc.migrationSessionId,
                                            migrationDoc.lsid,
                                            st.shard0,
                                            st.shard1));

    assert.soon(() => {
        let result = sendRecvChunkStatus(recipientPrimary, ns, migrationDoc.migrationSessionId);
        jsTestLog("recvChunkStatus: " + tojson(result));

        return result.state === "fail" &&
            result.errmsg.startsWith("migrate failed: TransactionTooOld:");
    });

    // Verify deletion task doesn't exist on recipient.
    assert.eq(recipientPrimary.getDB("config").rangeDeletions.find().itcount(), 0);
})();

st.stop();
})();
