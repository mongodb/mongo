/**
 * Test that removing and re-adding shard works correctly.
 *
 * This test is labeled resource intensive because its total io_write is 59MB compared to a median
 * of 5MB across all sharding tests in wiredTiger.
 * @tags: [resource_intensive, requires_persistence]
 */

// The UUID consistency check uses connections to shards cached on the ShardingTest object, but this
// test restarts a shard, so the cached connection is not usable.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

import {awaitRSClientHosts} from "jstests/replsets/rslib.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {moveOutSessionChunks, removeShard} from "jstests/sharding/libs/remove_shard_util.js";
import {moveDatabaseAndUnshardedColls} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

function seedString(replTest) {
    let members = replTest.getReplSetConfig().members.map(function (elem) {
        return elem.host;
    });
    return replTest.name + "/" + members.join(",");
}

// Await that each node in @nodes drop the RSM for @rsName
function awaitReplicaSetMonitorRemoval(nodes, rsName) {
    nodes.forEach(function (node) {
        jsTest.log("Awaiting ReplicaSetMonitor removal for " + rsName + " on " + node);
        assert.soon(
            function () {
                let replicaSets = assert.commandWorked(node.adminCommand("connPoolStats")).replicaSets;
                return !(rsName in replicaSets);
            },
            "Failed waiting for node " + node + "to remove ReplicaSetMonitor of replica set: " + rsName,
        );
    });
}

function setupInitialData(st, coll) {
    assert.commandWorked(
        st.s0.adminCommand({enableSharding: coll.getDB().getName(), primaryShard: st.shard0.shardName}),
    );

    assert.commandWorked(st.s0.adminCommand({shardCollection: coll.getFullName(), key: {i: 1}}));
    assert.commandWorked(st.splitAt(coll.getFullName(), {i: 5}));
    assert.commandWorked(st.moveChunk(coll.getFullName(), {i: 6}, st.shard1.shardName));
    assert.eq(
        1,
        findChunksUtil.countChunksForNs(st.s0.getDB("config"), coll.getFullName(), {shard: st.shard0.shardName}),
    );
    assert.eq(
        1,
        findChunksUtil.countChunksForNs(st.s0.getDB("config"), coll.getFullName(), {shard: st.shard1.shardName}),
    );

    let str = "a";
    while (str.length < 1024 * 16) {
        str += str;
    }

    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 300; i++) {
        bulk.insert({i: i % 10, str: str});
    }
    assert.commandWorked(bulk.execute());

    assert.eq(300, coll.find().itcount());
    st.printShardingStatus();
}

function removeShardAndCleanup(st, coll, replTest) {
    jsTest.log("Moving chunk from shard1 to shard0");
    assert.commandWorked(st.moveChunk(coll.getFullName(), {i: 6}, st.shard0.shardName));

    assert.eq(
        2,
        findChunksUtil.countChunksForNs(st.s0.getDB("config"), coll.getFullName(), {shard: st.shard0.shardName}),
    );
    assert.eq(
        0,
        findChunksUtil.countChunksForNs(st.s0.getDB("config"), coll.getFullName(), {shard: st.shard1.shardName}),
    );

    moveOutSessionChunks(st, replTest.name, st.shard0.shardName);

    jsTest.log("Removing shard with name: " + replTest.name);
    removeShard(st, replTest.name);

    // Drop the database so the shard can be re-added.
    assert.commandWorked(replTest.getPrimary().getDB(coll.getDB().getName()).dropDatabase());
}

function addShard(st, coll, replTest) {
    let seed = seedString(replTest);
    jsTest.log("Adding shard with seed: " + seed);
    assert.eq(true, st.adminCommand({addshard: seed, name: replTest.name}));
    awaitRSClientHosts(st.s, replTest.getPrimary(), {ok: true, ismaster: true});

    jsTest.log(`Moving chunk from shard0 to ${replTest.name}`);
    assert.commandWorked(st.moveChunk(coll.getFullName(), {i: 6}, replTest.name));
    assert.eq(
        1,
        findChunksUtil.countChunksForNs(st.s0.getDB("config"), coll.getFullName(), {shard: st.shard0.shardName}),
    );
    assert.eq(1, findChunksUtil.countChunksForNs(st.s0.getDB("config"), coll.getFullName(), {shard: replTest.name}));

    assert.eq(300, coll.find().itcount());
    jsTest.log("Shard added successfully");
}

let st = new ShardingTest({shards: {rs0: {nodes: 2}, rs1: {nodes: 2}}});
let coll = st.s.getCollection("test.remove2");

setupInitialData(st, coll);

jsTestLog("Test basic removal and re-addition of shard without shutting down.");

let rst1 = st.rs1;
removeShardAndCleanup(st, coll, rst1);
addShard(st, coll, rst1);

jsTestLog("Test basic removal and re-addition of shard with shutting down the replica set.");

const originalSeed = seedString(rst1);

removeShardAndCleanup(st, coll, rst1);
rst1.stopSet(null, true /* forRestart */);

// Await that both mongos and rs0 remove RSM for removed replicaset
awaitReplicaSetMonitorRemoval([st.s, st.rs0.getPrimary()], rst1.name);

rst1.startSet({restart: true});
rst1.awaitReplication();

assert.eq(originalSeed, seedString(rst1), "Set didn't come back up with the same hosts as before");
addShard(st, coll, rst1);

jsTestLog("Test removal and re-addition of shard with an identical replica set name and different port.");

removeShardAndCleanup(st, coll, rst1);
rst1.stopSet(null, true /* forRestart */);

// Await that both mongos and rs0 remove RSM for removed replicaset
awaitReplicaSetMonitorRemoval([st.s, st.rs0.getPrimary()], rst1.name);

let rst2 = new ReplSetTest({name: "dummy", nodes: 2, useHostName: true});
rst2.startSet({shardsvr: ""});
rst2.initiate();
rst2.awaitReplication();

addShard(st, coll, rst2);
assert.eq(300, coll.find().itcount());

jsTestLog("Verify that a database can be moved to the added shard.");
st.s.getDB("test2").foo.insert({a: 1});

moveDatabaseAndUnshardedColls(st.s.getDB("test2"), rst2.name);

assert.eq(1, st.s.getDB("test2").foo.find().itcount());

// Can't shut down with rst2 in the set or ShardingTest will fail trying to cleanup on shutdown.
// Have to take out rst2 and put rst1 back into the set so that it can clean up.
jsTestLog("Resetting the sharding test to its initial state to allow the test to shut down.");
moveDatabaseAndUnshardedColls(st.s.getDB("test2"), st.shard0.shardName);
removeShardAndCleanup(st, coll, rst2);
rst2.stopSet(null, true /* forRestart */);

// Await that both mongos and rs0 remove RSM for removed replicaset
awaitReplicaSetMonitorRemoval([st.s, st.rs0.getPrimary()], rst2.name);

rst1.startSet({restart: true});
rst1.awaitReplication();

assert.eq(originalSeed, seedString(rst1), "Set didn't come back up with the same hosts as before");
addShard(st, coll, rst1);

st.stop();
