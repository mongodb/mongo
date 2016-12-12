// This test requires persistence because it assumes standalone shards will still have their data
// after restarting.
// @tags: [requires_persistence]

(function() {
    'use strict';

    var st = new ShardingTest({name: "write_commands", mongos: 2, shards: 2});

    var dbTestName = 'WriteCommandsTestDB';
    var collName = dbTestName + '.TestColl';

    assert.commandWorked(st.s0.adminCommand({enablesharding: dbTestName}));
    st.ensurePrimaryShard(dbTestName, 'shard0000');

    assert.commandWorked(
        st.s0.adminCommand({shardCollection: collName, key: {Key: 1}, unique: true}));

    // Split at keys 10 and 20
    assert.commandWorked(st.s0.adminCommand({split: collName, middle: {Key: 10}}));
    assert.commandWorked(st.s0.adminCommand({split: collName, middle: {Key: 20}}));

    printjson(st.config.getSiblingDB('config').chunks.find().toArray());

    // Move 10 and 20 to shard00001
    assert.commandWorked(
        st.s0.adminCommand({moveChunk: collName, find: {Key: 19}, to: 'shard0001'}));
    assert.commandWorked(
        st.s0.adminCommand({moveChunk: collName, find: {Key: 21}, to: 'shard0001'}));

    printjson(st.config.getSiblingDB('config').chunks.find().toArray());

    // Insert one document in each chunk, which we will use to change
    assert(st.s1.getDB(dbTestName).TestColl.insert({Key: 1}));
    assert(st.s1.getDB(dbTestName).TestColl.insert({Key: 11}));
    assert(st.s1.getDB(dbTestName).TestColl.insert({Key: 21}));

    // Make sure the documents are correctly placed
    printjson(st.d0.getDB(dbTestName).TestColl.find().toArray());
    printjson(st.d1.getDB(dbTestName).TestColl.find().toArray());

    assert.eq(1, st.d0.getDB(dbTestName).TestColl.count());
    assert.eq(2, st.d1.getDB(dbTestName).TestColl.count());

    assert.eq(1, st.d0.getDB(dbTestName).TestColl.find({Key: 1}).count());
    assert.eq(1, st.d1.getDB(dbTestName).TestColl.find({Key: 11}).count());
    assert.eq(1, st.d1.getDB(dbTestName).TestColl.find({Key: 21}).count());

    // Move chunk [0, 19] to shard0000 and make sure the documents are correctly placed
    assert.commandWorked(
        st.s0.adminCommand({moveChunk: collName, find: {Key: 19}, to: 'shard0000'}));

    printjson(st.config.getSiblingDB('config').chunks.find().toArray());
    printjson(st.d0.getDB(dbTestName).TestColl.find({}).toArray());
    printjson(st.d1.getDB(dbTestName).TestColl.find({}).toArray());

    // Now restart all mongod instances, so they don't know yet that they are sharded
    st.restartMongod(0);
    st.restartMongod(1);

    // Now that both mongod shards are restarted, they don't know yet that they are part of a
    // sharded
    // cluster until they get a setShardVerion command. Mongos instance s1 has stale metadata and
    // doesn't know that chunk with key 19 has moved to shard0000 so it will send it to shard0001 at
    // first.
    //
    // Shard0001 would only send back a stale config exception if it receives a setShardVersion
    // command. The bug that this test validates is that setShardVersion is indeed being sent (for
    // more
    // information, see SERVER-19395).
    st.s1.getDB(dbTestName).TestColl.update({Key: 11}, {$inc: {Counter: 1}}, {upsert: true});

    printjson(st.d0.getDB(dbTestName).TestColl.find({}).toArray());
    printjson(st.d1.getDB(dbTestName).TestColl.find({}).toArray());

    assert.eq(2, st.d0.getDB(dbTestName).TestColl.count());
    assert.eq(1, st.d1.getDB(dbTestName).TestColl.count());

    assert.eq(1, st.d0.getDB(dbTestName).TestColl.find({Key: 1}).count());
    assert.eq(1, st.d0.getDB(dbTestName).TestColl.find({Key: 11}).count());
    assert.eq(1, st.d1.getDB(dbTestName).TestColl.find({Key: 21}).count());

    st.stop();

})();
