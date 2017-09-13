//
// Tests mongos's failure tolerance for single-node shards
//
// Sets up a cluster with three shards, the first shard of which has an unsharded collection and
// half a sharded collection.  The second shard has the second half of the sharded collection, and
// the third shard has nothing.  Progressively shuts down each shard to see the impact on the
// cluster.
//
// Three different connection states are tested - active (connection is active through whole
// sequence), idle (connection is connected but not used before a shard change), and new
// (connection connected after shard change).
//
(function() {
    'use strict';

    var st = new ShardingTest({shards: 3, mongos: 1});

    var admin = st.s0.getDB("admin");

    var collSharded = st.s0.getCollection("fooSharded.barSharded");
    var collUnsharded = st.s0.getCollection("fooUnsharded.barUnsharded");

    assert.commandWorked(admin.runCommand({enableSharding: collSharded.getDB().toString()}));
    st.ensurePrimaryShard(collSharded.getDB().toString(), st.shard0.shardName);

    assert.commandWorked(
        admin.runCommand({shardCollection: collSharded.toString(), key: {_id: 1}}));
    assert.commandWorked(admin.runCommand({split: collSharded.toString(), middle: {_id: 0}}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: collSharded.toString(), find: {_id: 0}, to: st.shard1.shardName}));

    // Create the unsharded database
    assert.writeOK(collUnsharded.insert({some: "doc"}));
    assert.writeOK(collUnsharded.remove({}));
    st.ensurePrimaryShard(collUnsharded.getDB().toString(), st.shard0.shardName);

    //
    // Setup is complete
    //

    jsTest.log("Inserting initial data...");

    var mongosConnActive = new Mongo(st.s0.host);
    var mongosConnIdle = null;
    var mongosConnNew = null;

    assert.writeOK(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -1}));
    assert.writeOK(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 1}));
    assert.writeOK(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 1}));

    jsTest.log("Stopping third shard...");

    mongosConnIdle = new Mongo(st.s0.host);

    MongoRunner.stopMongod(st.shard2);

    jsTest.log("Testing active connection...");

    assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    assert.writeOK(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -2}));
    assert.writeOK(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 2}));
    assert.writeOK(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 2}));

    jsTest.log("Testing idle connection...");

    assert.writeOK(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -3}));
    assert.writeOK(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 3}));
    assert.writeOK(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 3}));

    assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    jsTest.log("Testing new connections...");

    mongosConnNew = new Mongo(st.s0.host);
    assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    mongosConnNew = new Mongo(st.s0.host);
    assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    mongosConnNew = new Mongo(st.s0.host);
    assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    mongosConnNew = new Mongo(st.s0.host);
    assert.writeOK(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -4}));
    mongosConnNew = new Mongo(st.s0.host);
    assert.writeOK(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 4}));
    mongosConnNew = new Mongo(st.s0.host);
    assert.writeOK(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 4}));

    gc();  // Clean up new connections

    jsTest.log("Stopping second shard...");

    mongosConnIdle = new Mongo(st.s0.host);

    MongoRunner.stopMongod(st.shard1);

    jsTest.log("Testing active connection...");

    assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    assert.writeOK(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -5}));

    assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 5}));
    assert.writeOK(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 5}));

    jsTest.log("Testing idle connection...");

    assert.writeOK(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -6}));
    assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 6}));
    assert.writeOK(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 6}));

    assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    jsTest.log("Testing new connections...");

    mongosConnNew = new Mongo(st.s0.host);
    assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));

    mongosConnNew = new Mongo(st.s0.host);
    assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    mongosConnNew = new Mongo(st.s0.host);
    assert.writeOK(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -7}));

    mongosConnNew = new Mongo(st.s0.host);
    assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 7}));

    mongosConnNew = new Mongo(st.s0.host);
    assert.writeOK(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 7}));

    st.stop();

})();
