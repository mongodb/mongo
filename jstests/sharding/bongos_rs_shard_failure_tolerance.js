//
// Tests bongos's failure tolerance for replica set shards and read preference queries
//
// Sets up a cluster with three shards, the first shard of which has an unsharded collection and
// half a sharded collection.  The second shard has the second half of the sharded collection, and
// the third shard has nothing.  Progressively shuts down the primary of each shard to see the
// impact on the cluster.
//
// Three different connection states are tested - active (connection is active through whole
// sequence), idle (connection is connected but not used before a shard change), and new
// (connection connected after shard change).
//
(function() {
    'use strict';

    var st = new ShardingTest({shards: 3, bongos: 1, other: {rs: true, rsOptions: {nodes: 2}}});

    var bongos = st.s0;
    var admin = bongos.getDB("admin");
    var shards = bongos.getDB("config").shards.find().toArray();

    assert.commandWorked(admin.runCommand({setParameter: 1, traceExceptions: true}));

    var collSharded = bongos.getCollection("fooSharded.barSharded");
    var collUnsharded = bongos.getCollection("fooUnsharded.barUnsharded");

    // Create the unsharded database
    assert.writeOK(collUnsharded.insert({some: "doc"}));
    assert.writeOK(collUnsharded.remove({}));
    printjson(admin.runCommand({movePrimary: collUnsharded.getDB().toString(), to: shards[0]._id}));

    // Create the sharded database
    assert.commandWorked(admin.runCommand({enableSharding: collSharded.getDB().toString()}));
    printjson(admin.runCommand({movePrimary: collSharded.getDB().toString(), to: shards[0]._id}));
    assert.commandWorked(
        admin.runCommand({shardCollection: collSharded.toString(), key: {_id: 1}}));
    assert.commandWorked(admin.runCommand({split: collSharded.toString(), middle: {_id: 0}}));
    assert.commandWorked(
        admin.runCommand({moveChunk: collSharded.toString(), find: {_id: 0}, to: shards[1]._id}));

    st.printShardingStatus();

    //
    // Setup is complete
    //

    jsTest.log("Inserting initial data...");

    var bongosConnActive = new Bongo(bongos.host);
    var bongosConnIdle = null;
    var bongosConnNew = null;

    var wc = {writeConcern: {w: 2, wtimeout: 60000}};

    assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -1}, wc));
    assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 1}, wc));
    assert.writeOK(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 1}, wc));

    jsTest.log("Stopping primary of third shard...");

    bongosConnIdle = new Bongo(bongos.host);

    st.rs2.stop(st.rs2.getPrimary());

    jsTest.log("Testing active connection with third primary down...");

    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -2}, wc));
    assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 2}, wc));
    assert.writeOK(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 2}, wc));

    jsTest.log("Testing idle connection with third primary down...");

    assert.writeOK(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: -3}, wc));
    assert.writeOK(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: 3}, wc));
    assert.writeOK(bongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 3}, wc));

    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    jsTest.log("Testing new connections with third primary down...");

    bongosConnNew = new Bongo(bongos.host);
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    bongosConnNew = new Bongo(bongos.host);
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    bongosConnNew = new Bongo(bongos.host);
    assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    bongosConnNew = new Bongo(bongos.host);
    assert.writeOK(bongosConnNew.getCollection(collSharded.toString()).insert({_id: -4}, wc));
    bongosConnNew = new Bongo(bongos.host);
    assert.writeOK(bongosConnNew.getCollection(collSharded.toString()).insert({_id: 4}, wc));
    bongosConnNew = new Bongo(bongos.host);
    assert.writeOK(bongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 4}, wc));

    gc();  // Clean up new connections

    jsTest.log("Stopping primary of second shard...");

    bongosConnIdle = new Bongo(bongos.host);

    // Need to save this node for later
    var rs1Secondary = st.rs1.getSecondary();

    st.rs1.stop(st.rs1.getPrimary());

    jsTest.log("Testing active connection with second primary down...");

    // Reads with read prefs
    bongosConnActive.setSlaveOk();
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));
    bongosConnActive.setSlaveOk(false);

    bongosConnActive.setReadPref("primary");
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.throws(function() {
        bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1});
    });
    assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    // Ensure read prefs override slaveOK
    bongosConnActive.setSlaveOk();
    bongosConnActive.setReadPref("primary");
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.throws(function() {
        bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1});
    });
    assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));
    bongosConnActive.setSlaveOk(false);

    bongosConnActive.setReadPref("secondary");
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    bongosConnActive.setReadPref("primaryPreferred");
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    bongosConnActive.setReadPref("secondaryPreferred");
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    bongosConnActive.setReadPref("nearest");
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    // Writes
    assert.writeOK(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -5}, wc));
    assert.writeError(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 5}, wc));
    assert.writeOK(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 5}, wc));

    jsTest.log("Testing idle connection with second primary down...");

    // Writes
    assert.writeOK(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: -6}, wc));
    assert.writeError(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: 6}, wc));
    assert.writeOK(bongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 6}, wc));

    // Reads with read prefs
    bongosConnIdle.setSlaveOk();
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));
    bongosConnIdle.setSlaveOk(false);

    bongosConnIdle.setReadPref("primary");
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.throws(function() {
        bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1});
    });
    assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    // Ensure read prefs override slaveOK
    bongosConnIdle.setSlaveOk();
    bongosConnIdle.setReadPref("primary");
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.throws(function() {
        bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1});
    });
    assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));
    bongosConnIdle.setSlaveOk(false);

    bongosConnIdle.setReadPref("secondary");
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    bongosConnIdle.setReadPref("primaryPreferred");
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    bongosConnIdle.setReadPref("secondaryPreferred");
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    bongosConnIdle.setReadPref("nearest");
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    jsTest.log("Testing new connections with second primary down...");

    // Reads with read prefs
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setSlaveOk();
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setSlaveOk();
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setSlaveOk();
    assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("primary");
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("primary");
    assert.throws(function() {
        bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1});
    });
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("primary");
    assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    // Ensure read prefs override slaveok
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setSlaveOk();
    bongosConnNew.setReadPref("primary");
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setSlaveOk();
    bongosConnNew.setReadPref("primary");
    assert.throws(function() {
        bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1});
    });
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setSlaveOk();
    bongosConnNew.setReadPref("primary");
    assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("secondary");
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("secondary");
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("secondary");
    assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("primaryPreferred");
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("primaryPreferred");
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("primaryPreferred");
    assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("secondaryPreferred");
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("secondaryPreferred");
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("secondaryPreferred");
    assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("nearest");
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("nearest");
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setReadPref("nearest");
    assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    // Writes
    bongosConnNew = new Bongo(bongos.host);
    assert.writeOK(bongosConnNew.getCollection(collSharded.toString()).insert({_id: -7}, wc));
    bongosConnNew = new Bongo(bongos.host);
    assert.writeError(bongosConnNew.getCollection(collSharded.toString()).insert({_id: 7}, wc));
    bongosConnNew = new Bongo(bongos.host);
    assert.writeOK(bongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 7}, wc));

    gc();  // Clean up new connections

    jsTest.log("Stopping primary of first shard...");

    bongosConnIdle = new Bongo(bongos.host);

    st.rs0.stop(st.rs0.getPrimary());

    jsTest.log("Testing active connection with first primary down...");

    bongosConnActive.setSlaveOk();
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    assert.writeError(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -8}));
    assert.writeError(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 8}));
    assert.writeError(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 8}));

    jsTest.log("Testing idle connection with first primary down...");

    assert.writeError(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: -9}));
    assert.writeError(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: 9}));
    assert.writeError(bongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 9}));

    bongosConnIdle.setSlaveOk();
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    jsTest.log("Testing new connections with first primary down...");

    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setSlaveOk();
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setSlaveOk();
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setSlaveOk();
    assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    bongosConnNew = new Bongo(bongos.host);
    assert.writeError(bongosConnNew.getCollection(collSharded.toString()).insert({_id: -10}));
    bongosConnNew = new Bongo(bongos.host);
    assert.writeError(bongosConnNew.getCollection(collSharded.toString()).insert({_id: 10}));
    bongosConnNew = new Bongo(bongos.host);
    assert.writeError(bongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 10}));

    gc();  // Clean up new connections

    jsTest.log("Stopping second shard...");

    bongosConnIdle = new Bongo(bongos.host);

    st.rs1.stop(rs1Secondary);

    jsTest.log("Testing active connection with second shard down...");

    bongosConnActive.setSlaveOk();
    assert.neq(null, bongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    assert.writeError(bongosConnActive.getCollection(collSharded.toString()).insert({_id: -11}));
    assert.writeError(bongosConnActive.getCollection(collSharded.toString()).insert({_id: 11}));
    assert.writeError(bongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 11}));

    jsTest.log("Testing idle connection with second shard down...");

    assert.writeError(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: -12}));
    assert.writeError(bongosConnIdle.getCollection(collSharded.toString()).insert({_id: 12}));
    assert.writeError(bongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 12}));

    bongosConnIdle.setSlaveOk();
    assert.neq(null, bongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, bongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    jsTest.log("Testing new connections with second shard down...");

    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setSlaveOk();
    assert.neq(null, bongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    bongosConnNew = new Bongo(bongos.host);
    bongosConnNew.setSlaveOk();
    assert.neq(null, bongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    bongosConnNew = new Bongo(bongos.host);
    assert.writeError(bongosConnNew.getCollection(collSharded.toString()).insert({_id: -13}));
    bongosConnNew = new Bongo(bongos.host);
    assert.writeError(bongosConnNew.getCollection(collSharded.toString()).insert({_id: 13}));
    bongosConnNew = new Bongo(bongos.host);
    assert.writeError(bongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 13}));

    gc();  // Clean up new connections

    st.stop();

})();
