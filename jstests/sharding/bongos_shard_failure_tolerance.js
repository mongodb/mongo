//
// Tests bongos's failure tolerance for single-node shards
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

    var st = new ShardingTest({shards: 3, bongos: 1});

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

    var bongosConnActive = new Bongo(st.s0.host);
    var bongosConnIdle = null;
    var bongosConnNew = null;

    assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -1}));
    assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 1}));
    assert.writeOK(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 1}));

    jsTest.log("Stopping third shard...");

    bongosConnIdle = new Bongo(st.s0.host);

    BongoRunner.stopBongod(st.shard2);

    jsTest.log("Testing active connection...");

    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -2}));
    assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 2}));
    assert.writeOK(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 2}));

    jsTest.log("Testing idle connection...");

    assert.writeOK(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: -3}));
    assert.writeOK(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: 3}));
    assert.writeOK(bongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 3}));

    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    jsTest.log("Testing new connections...");

    bongosConnNew = new Bongo(st.s0.host);
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    bongosConnNew = new Bongo(st.s0.host);
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    bongosConnNew = new Bongo(st.s0.host);
    assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    bongosConnNew = new Bongo(st.s0.host);
    assert.writeOK(bongosConnNew.getCollection(collSharded.toString()).insert({_id: -4}));
    bongosConnNew = new Bongo(st.s0.host);
    assert.writeOK(bongosConnNew.getCollection(collSharded.toString()).insert({_id: 4}));
    bongosConnNew = new Bongo(st.s0.host);
    assert.writeOK(bongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 4}));

    gc();  // Clean up new connections

    jsTest.log("Stopping second shard...");

    bongosConnIdle = new Bongo(st.s0.host);

    BongoRunner.stopBongod(st.shard1);

    jsTest.log("Testing active connection...");

    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -5}));

    assert.writeError(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 5}));
    assert.writeOK(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 5}));

    jsTest.log("Testing idle connection...");

    assert.writeOK(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: -6}));
    assert.writeError(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: 6}));
    assert.writeOK(bongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 6}));

    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    jsTest.log("Testing new connections...");

    bongosConnNew = new Bongo(st.s0.host);
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));

    bongosConnNew = new Bongo(st.s0.host);
    assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    bongosConnNew = new Bongo(st.s0.host);
    assert.writeOK(bongosConnNew.getCollection(collSharded.toString()).insert({_id: -7}));

    bongosConnNew = new Bongo(st.s0.host);
    assert.writeError(bongosConnNew.getCollection(collSharded.toString()).insert({_id: 7}));

    bongosConnNew = new Bongo(st.s0.host);
    assert.writeOK(bongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 7}));

    st.stop();

})();
