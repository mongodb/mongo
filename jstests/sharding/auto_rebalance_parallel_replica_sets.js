/**
 * Tests that the cluster is balanced in parallel in one balancer round (replica sets).
 */
(function() {
    'use strict';

    var st = new ShardingTest({shards: 4, rs: {nodes: 3}});

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'TestDB'}));
    st.ensurePrimaryShard('TestDB', st.shard0.shardName);
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'TestDB.TestColl', key: {Key: 1}}));

    var coll = st.s0.getDB('TestDB').TestColl;

    // Create 4 chunks initially and ensure they get balanced within 1 balancer round
    assert.writeOK(coll.insert({Key: 1, Value: 'Test value 1'}));
    assert.writeOK(coll.insert({Key: 10, Value: 'Test value 10'}));
    assert.writeOK(coll.insert({Key: 20, Value: 'Test value 20'}));
    assert.writeOK(coll.insert({Key: 30, Value: 'Test value 30'}));

    assert.commandWorked(st.splitAt('TestDB.TestColl', {Key: 10}));
    assert.commandWorked(st.splitAt('TestDB.TestColl', {Key: 20}));
    assert.commandWorked(st.splitAt('TestDB.TestColl', {Key: 30}));

    // Move two of the chunks to shard0001 so we have option to do parallel balancing
    assert.commandWorked(st.moveChunk('TestDB.TestColl', {Key: 20}, st.shard1.shardName));
    assert.commandWorked(st.moveChunk('TestDB.TestColl', {Key: 30}, st.shard1.shardName));

    assert.eq(2, st.s0.getDB('config').chunks.find({shard: st.shard0.shardName}).itcount());
    assert.eq(2, st.s0.getDB('config').chunks.find({shard: st.shard1.shardName}).itcount());

    // Do enable the balancer and wait for a single balancer round
    st.startBalancer();
    st.awaitBalancerRound();
    st.stopBalancer();

    assert.eq(1, st.s0.getDB('config').chunks.find({shard: st.shard0.shardName}).itcount());
    assert.eq(1, st.s0.getDB('config').chunks.find({shard: st.shard1.shardName}).itcount());
    assert.eq(1, st.s0.getDB('config').chunks.find({shard: st.shard2.shardName}).itcount());
    assert.eq(1, st.s0.getDB('config').chunks.find({shard: st.shard3.shardName}).itcount());

    // Ensure the range deleter quiesces
    st.rs0.awaitReplication();
    st.rs1.awaitReplication();
    st.rs2.awaitReplication();
    st.rs3.awaitReplication();

    st.stop();
})();
