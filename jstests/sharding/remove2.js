/**
 * Test that removing and re-adding shard works correctly.
 *
 * This test is labeled resource intensive because its total io_write is 59MB compared to a median
 * of 5MB across all sharding tests in wiredTiger.
 * @tags: [
 *   resource_intensive,
 *   need_fixing_for_46
 * ]
 */

// The UUID consistency check uses connections to shards cached on the ShardingTest object, but this
// test restarts a shard, so the cached connection is not usable.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
'use strict';
load("jstests/replsets/rslib.js");

function seedString(replTest) {
    var members = replTest.getReplSetConfig().members.map(function(elem) {
        return elem.host;
    });
    return replTest.name + '/' + members.join(',');
}

function awaitReplicaSetMonitorTimeout() {
    print("Sleeping for 60 seconds to let the other shard's ReplicaSetMonitor time out");
    sleep(60000);  // 60s should be plenty since the ReplicaSetMonitor refreshes every 30s.
}

function setupInitialData(st, coll) {
    coll.drop();

    assert.commandWorked(st.s0.adminCommand({enableSharding: coll.getDB().getName()}));
    st.ensurePrimaryShard(coll.getDB().getName(), st.shard0.shardName);

    assert.commandWorked(st.s0.adminCommand({shardCollection: coll.getFullName(), key: {i: 1}}));
    assert.commandWorked(st.splitAt(coll.getFullName(), {i: 5}));
    assert.commandWorked(st.moveChunk(coll.getFullName(), {i: 6}, st.shard1.shardName));
    assert.eq(
        1,
        st.s0.getDB('config').chunks.count({ns: coll.getFullName(), shard: st.shard0.shardName}));
    assert.eq(
        1,
        st.s0.getDB('config').chunks.count({ns: coll.getFullName(), shard: st.shard1.shardName}));

    let str = 'a';
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

function removeShard(st, coll, replTest) {
    jsTest.log("Removing shard with name: " + replTest.name);

    assert.commandWorked(st.moveChunk(coll.getFullName(), {i: 6}, st.shard0.shardName));

    assert.eq(
        2,
        st.s0.getDB('config').chunks.count({ns: coll.getFullName(), shard: st.shard0.shardName}));
    assert.eq(
        0,
        st.s0.getDB('config').chunks.count({ns: coll.getFullName(), shard: st.shard1.shardName}));

    var res = st.s.adminCommand({removeShard: replTest.name});
    assert.commandWorked(res);
    assert.eq('started', res.state);
    assert.soon(function() {
        res = st.s.adminCommand({removeShard: replTest.name});
        assert.commandWorked(res);
        return ('completed' === res.state);
    }, "failed to remove shard: " + tojson(res));

    // Drop the database so the shard can be re-added.
    assert.commandWorked(replTest.getPrimary().getDB(coll.getDB().getName()).dropDatabase());
}

function addShard(st, coll, replTest) {
    var seed = seedString(replTest);
    print("Adding shard with seed: " + seed);
    assert.eq(true, st.adminCommand({addshard: seed}));
    awaitRSClientHosts(
        new Mongo(st.s.host), replTest.getSecondaries(), {ok: true, secondary: true});

    assert.commandWorked(st.moveChunk(coll.getFullName(), {i: 6}, st.shard1.shardName));
    assert.eq(
        1,
        st.s0.getDB('config').chunks.count({ns: coll.getFullName(), shard: st.shard0.shardName}));
    assert.eq(
        1,
        st.s0.getDB('config').chunks.count({ns: coll.getFullName(), shard: st.shard1.shardName}));

    assert.eq(300, coll.find().itcount());
    print("Shard added successfully");
}

let st = new ShardingTest({shards: {rs0: {nodes: 2}, rs1: {nodes: 2}}});
let conn = new Mongo(st.s.host);
let coll = conn.getCollection("test.remove2");

setupInitialData(st, coll);

jsTestLog("Test basic removal and re-addition of shard without shutting down.");

let rst1 = st.rs1;
removeShard(st, coll, rst1);
addShard(st, coll, rst1);

jsTestLog("Test basic removal and re-addition of shard with shutting down the replica set.");

const originalSeed = seedString(rst1);

removeShard(st, coll, rst1);
rst1.stopSet();
rst1.startSet({restart: true});
rst1.initiate();
rst1.awaitReplication();

assert.eq(originalSeed, seedString(rst1), "Set didn't come back up with the same hosts as before");
addShard(st, coll, rst1);

jsTestLog(
    "Test removal and re-addition of shard with an identical replica set name and different port.");

removeShard(st, coll, rst1);
rst1.stopSet();

awaitReplicaSetMonitorTimeout();

let rst2 = new ReplSetTest({name: rst1.name, nodes: 2, useHostName: true});
rst2.startSet({shardsvr: ""});
rst2.initiate();
rst2.awaitReplication();

addShard(st, coll, rst2);
assert.eq(300, coll.find().itcount());

jsTestLog("Verify that a database can be moved to the added shard.");
conn.getDB('test2').foo.insert({a: 1});
assert.commandWorked(st.admin.runCommand({movePrimary: 'test2', to: rst2.name}));
assert.eq(1, conn.getDB('test2').foo.find().itcount());

// Can't shut down with rst2 in the set or ShardingTest will fail trying to cleanup on shutdown.
// Have to take out rst2 and put rst1 back into the set so that it can clean up.
jsTestLog("Resetting the sharding test to its initial state to allow the test to shut down.");
assert.commandWorked(st.admin.runCommand({movePrimary: 'test2', to: st.rs0.name}));
removeShard(st, coll, rst2);
rst2.stopSet();

awaitReplicaSetMonitorTimeout();

rst1.startSet({restart: true});
rst1.initiate();
rst1.awaitReplication();

assert.eq(originalSeed, seedString(rst1), "Set didn't come back up with the same hosts as before");
addShard(st, coll, rst1);

st.stop();
})();
