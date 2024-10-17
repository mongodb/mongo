/*
 * Tests that 'approxDocumentsToCopy' and 'approxBytesToCopy' are correctly restored after failover.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function checkMoveCollectionCloningMetrics(
    st, ns, numDocs, numBytes, primaryShardName, toShardName) {
    assert.neq(primaryShardName, toShardName);
    let currentOps;
    assert.soon(() => {
        currentOps = st.s.getDB("admin")
                         .aggregate([
                             {$currentOp: {allUsers: true, localOps: false}},
                             {
                                 $match: {
                                     type: "op",
                                     "originatingCommand.reshardCollection": ns,
                                     recipientState: {$exists: true}
                                 }
                             },
                         ])
                         .toArray();
        if (currentOps.length < 2) {
            return false;
        }
        for (let op of currentOps) {
            if (op.recipientState != "cloning") {
                return false;
            }
        }
        return true;
    }, () => tojson(currentOps));

    assert.eq(currentOps.length, 2, currentOps);
    currentOps.forEach(op => {
        if (op.shard == primaryShardName) {
            assert.eq(op.approxDocumentsToCopy, 0, {op});
            assert.eq(op.approxBytesToCopy, 0, {op});
        } else if (op.shard == toShardName) {
            assert.eq(op.approxDocumentsToCopy, numDocs, {op});
            assert.eq(op.approxBytesToCopy, numBytes, {op});
        } else {
            throw Error("Unexpected shard name " + tojson(op));
        }
    });
}

function runTest() {
    const st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 2}});

    // Create an unsharded collection on shard0 (primary shard) and move the collection from
    // shard0 to shard1.
    const dbName = "testDb";
    const collName = "testColl";
    const ns = dbName + "." + collName;
    const coll = st.s.getCollection(ns);

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    const numDocs = 100;
    const docs = [];
    for (let i = 0; i < numDocs; i++) {
        const doc = {_id: i, x: i};
        docs.push(doc);
    }
    const numBytes = numDocs * Object.bsonsize({_id: 0, x: 0});
    assert.commandWorked(coll.insert(docs));

    // Pause resharding recipients (both shard0 and shard1) at the "cloning" state.
    const shard0CloningFps =
        st.rs0.nodes.map(node => configureFailPoint(node, "reshardingPauseRecipientBeforeCloning"));
    const shard1CloningFps =
        st.rs1.nodes.map(node => configureFailPoint(node, "reshardingPauseRecipientBeforeCloning"));

    const thread = new Thread((host, ns, toShard) => {
        const mongos = new Mongo(host);
        assert.soonRetryOnAcceptableErrors(() => {
            const res = mongos.adminCommand({moveCollection: ns, toShard});
            assert.commandWorked(res);
            return true;
        }, ErrorCodes.FailedToSatisfyReadPreference);
    }, st.s.host, ns, st.shard1.shardName);
    thread.start();

    checkMoveCollectionCloningMetrics(st,
                                      ns,
                                      numDocs,
                                      numBytes,
                                      st.shard0.shardName /* primaryShard */,
                                      st.shard1.shardName /* toShard */);

    // Trigger a failover on shard0.
    const oldShard0Primary = st.rs0.getPrimary();
    assert.commandWorked(
        oldShard0Primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    assert.commandWorked(oldShard0Primary.adminCommand({replSetFreeze: 0}));
    st.rs0.waitForPrimary();

    // Trigger a failover on shard1.
    const oldShard1Primary = st.rs1.getPrimary();
    assert.commandWorked(
        oldShard1Primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    assert.commandWorked(oldShard1Primary.adminCommand({replSetFreeze: 0}));
    st.rs1.waitForPrimary();

    checkMoveCollectionCloningMetrics(st,
                                      ns,
                                      numDocs,
                                      numBytes,
                                      st.shard0.shardName /* primaryShard */,
                                      st.shard1.shardName /* toShard */);
    shard0CloningFps.forEach(fp => fp.off());
    shard1CloningFps.forEach(fp => fp.off());
    thread.join();

    assert(coll.drop());
    st.stop();
}

runTest();
