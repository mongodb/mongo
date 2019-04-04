//
// Tests merizos's failure tolerance for replica set shards and read preference queries
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

// Checking UUID consistency involves talking to shard primaries, but by the end of this test, one
// shard does not have a primary.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    'use strict';

    var st = new ShardingTest({shards: 3, merizos: 1, other: {rs: true, rsOptions: {nodes: 2}}});

    var merizos = st.s0;
    var admin = merizos.getDB("admin");

    assert.commandWorked(admin.runCommand({setParameter: 1, traceExceptions: true}));

    var collSharded = merizos.getCollection("fooSharded.barSharded");
    var collUnsharded = merizos.getCollection("fooUnsharded.barUnsharded");

    // Create the unsharded database
    assert.writeOK(collUnsharded.insert({some: "doc"}));
    assert.writeOK(collUnsharded.remove({}));
    assert.commandWorked(
        admin.runCommand({movePrimary: collUnsharded.getDB().toString(), to: st.shard0.shardName}));

    // Create the sharded database
    assert.commandWorked(admin.runCommand({enableSharding: collSharded.getDB().toString()}));
    assert.commandWorked(
        admin.runCommand({movePrimary: collSharded.getDB().toString(), to: st.shard0.shardName}));
    assert.commandWorked(
        admin.runCommand({shardCollection: collSharded.toString(), key: {_id: 1}}));
    assert.commandWorked(admin.runCommand({split: collSharded.toString(), middle: {_id: 0}}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: collSharded.toString(), find: {_id: 0}, to: st.shard1.shardName}));

    // Secondaries do not refresh their in-memory routing table until a request with a higher
    // version is received, and refreshing requires communication with the primary to obtain the
    // newest version. Read from the secondaries once before taking down primaries to ensure they
    // have loaded the routing table into memory.
    // TODO SERVER-30148: replace this with calls to awaitReplication() on each shard owning data
    // for the sharded collection once secondaries refresh proactively.
    var merizosSetupConn = new Merizo(merizos.host);
    merizosSetupConn.setReadPref("secondary");
    assert(!merizosSetupConn.getCollection(collSharded.toString()).find({}).hasNext());

    gc();  // Clean up connections

    st.printShardingStatus();

    //
    // Setup is complete
    //

    jsTest.log("Inserting initial data...");

    var merizosConnActive = new Merizo(merizos.host);
    var merizosConnIdle = null;
    var merizosConnNew = null;

    var wc = {writeConcern: {w: 2, wtimeout: 60000}};

    assert.writeOK(merizosConnActive.getCollection(collSharded.toString()).insert({_id: -1}, wc));
    assert.writeOK(merizosConnActive.getCollection(collSharded.toString()).insert({_id: 1}, wc));
    assert.writeOK(merizosConnActive.getCollection(collUnsharded.toString()).insert({_id: 1}, wc));

    jsTest.log("Stopping primary of third shard...");

    merizosConnIdle = new Merizo(merizos.host);

    st.rs2.stop(st.rs2.getPrimary());

    jsTest.log("Testing active connection with third primary down...");

    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    assert.writeOK(merizosConnActive.getCollection(collSharded.toString()).insert({_id: -2}, wc));
    assert.writeOK(merizosConnActive.getCollection(collSharded.toString()).insert({_id: 2}, wc));
    assert.writeOK(merizosConnActive.getCollection(collUnsharded.toString()).insert({_id: 2}, wc));

    jsTest.log("Testing idle connection with third primary down...");

    assert.writeOK(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: -3}, wc));
    assert.writeOK(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: 3}, wc));
    assert.writeOK(merizosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 3}, wc));

    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    jsTest.log("Testing new connections with third primary down...");

    merizosConnNew = new Merizo(merizos.host);
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    merizosConnNew = new Merizo(merizos.host);
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    merizosConnNew = new Merizo(merizos.host);
    assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    merizosConnNew = new Merizo(merizos.host);
    assert.writeOK(merizosConnNew.getCollection(collSharded.toString()).insert({_id: -4}, wc));
    merizosConnNew = new Merizo(merizos.host);
    assert.writeOK(merizosConnNew.getCollection(collSharded.toString()).insert({_id: 4}, wc));
    merizosConnNew = new Merizo(merizos.host);
    assert.writeOK(merizosConnNew.getCollection(collUnsharded.toString()).insert({_id: 4}, wc));

    gc();  // Clean up new connections

    jsTest.log("Stopping primary of second shard...");

    merizosConnIdle = new Merizo(merizos.host);

    // Need to save this node for later
    var rs1Secondary = st.rs1.getSecondary();

    st.rs1.stop(st.rs1.getPrimary());

    jsTest.log("Testing active connection with second primary down...");

    // Reads with read prefs
    merizosConnActive.setSlaveOk();
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));
    merizosConnActive.setSlaveOk(false);

    merizosConnActive.setReadPref("primary");
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.throws(function() {
        merizosConnActive.getCollection(collSharded.toString()).findOne({_id: 1});
    });
    assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    // Ensure read prefs override slaveOK
    merizosConnActive.setSlaveOk();
    merizosConnActive.setReadPref("primary");
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.throws(function() {
        merizosConnActive.getCollection(collSharded.toString()).findOne({_id: 1});
    });
    assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));
    merizosConnActive.setSlaveOk(false);

    merizosConnActive.setReadPref("secondary");
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    merizosConnActive.setReadPref("primaryPreferred");
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    merizosConnActive.setReadPref("secondaryPreferred");
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    merizosConnActive.setReadPref("nearest");
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    // Writes
    assert.writeOK(merizosConnActive.getCollection(collSharded.toString()).insert({_id: -5}, wc));
    assert.writeError(merizosConnActive.getCollection(collSharded.toString()).insert({_id: 5}, wc));
    assert.writeOK(merizosConnActive.getCollection(collUnsharded.toString()).insert({_id: 5}, wc));

    jsTest.log("Testing idle connection with second primary down...");

    // Writes
    assert.writeOK(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: -6}, wc));
    assert.writeError(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: 6}, wc));
    assert.writeOK(merizosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 6}, wc));

    // Reads with read prefs
    merizosConnIdle.setSlaveOk();
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));
    merizosConnIdle.setSlaveOk(false);

    merizosConnIdle.setReadPref("primary");
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.throws(function() {
        merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1});
    });
    assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    // Ensure read prefs override slaveOK
    merizosConnIdle.setSlaveOk();
    merizosConnIdle.setReadPref("primary");
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.throws(function() {
        merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1});
    });
    assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));
    merizosConnIdle.setSlaveOk(false);

    merizosConnIdle.setReadPref("secondary");
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    merizosConnIdle.setReadPref("primaryPreferred");
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    merizosConnIdle.setReadPref("secondaryPreferred");
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    merizosConnIdle.setReadPref("nearest");
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    jsTest.log("Testing new connections with second primary down...");

    // Reads with read prefs
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setSlaveOk();
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setSlaveOk();
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setSlaveOk();
    assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("primary");
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("primary");
    assert.throws(function() {
        merizosConnNew.getCollection(collSharded.toString()).findOne({_id: 1});
    });
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("primary");
    assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    // Ensure read prefs override slaveok
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setSlaveOk();
    merizosConnNew.setReadPref("primary");
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setSlaveOk();
    merizosConnNew.setReadPref("primary");
    assert.throws(function() {
        merizosConnNew.getCollection(collSharded.toString()).findOne({_id: 1});
    });
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setSlaveOk();
    merizosConnNew.setReadPref("primary");
    assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("secondary");
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("secondary");
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("secondary");
    assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("primaryPreferred");
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("primaryPreferred");
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("primaryPreferred");
    assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("secondaryPreferred");
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("secondaryPreferred");
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("secondaryPreferred");
    assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("nearest");
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("nearest");
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setReadPref("nearest");
    assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    gc();  // Clean up new connections incrementally to compensate for slow win32 machine.

    // Writes
    merizosConnNew = new Merizo(merizos.host);
    assert.writeOK(merizosConnNew.getCollection(collSharded.toString()).insert({_id: -7}, wc));
    merizosConnNew = new Merizo(merizos.host);
    assert.writeError(merizosConnNew.getCollection(collSharded.toString()).insert({_id: 7}, wc));
    merizosConnNew = new Merizo(merizos.host);
    assert.writeOK(merizosConnNew.getCollection(collUnsharded.toString()).insert({_id: 7}, wc));

    gc();  // Clean up new connections

    jsTest.log("Stopping primary of first shard...");

    merizosConnIdle = new Merizo(merizos.host);

    st.rs0.stop(st.rs0.getPrimary());

    jsTest.log("Testing active connection with first primary down...");

    merizosConnActive.setSlaveOk();
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    assert.writeError(merizosConnActive.getCollection(collSharded.toString()).insert({_id: -8}));
    assert.writeError(merizosConnActive.getCollection(collSharded.toString()).insert({_id: 8}));
    assert.writeError(merizosConnActive.getCollection(collUnsharded.toString()).insert({_id: 8}));

    jsTest.log("Testing idle connection with first primary down...");

    assert.writeError(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: -9}));
    assert.writeError(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: 9}));
    assert.writeError(merizosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 9}));

    merizosConnIdle.setSlaveOk();
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
    assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    jsTest.log("Testing new connections with first primary down...");

    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setSlaveOk();
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setSlaveOk();
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setSlaveOk();
    assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    merizosConnNew = new Merizo(merizos.host);
    assert.writeError(merizosConnNew.getCollection(collSharded.toString()).insert({_id: -10}));
    merizosConnNew = new Merizo(merizos.host);
    assert.writeError(merizosConnNew.getCollection(collSharded.toString()).insert({_id: 10}));
    merizosConnNew = new Merizo(merizos.host);
    assert.writeError(merizosConnNew.getCollection(collUnsharded.toString()).insert({_id: 10}));

    gc();  // Clean up new connections

    jsTest.log("Stopping second shard...");

    merizosConnIdle = new Merizo(merizos.host);

    st.rs1.stop(rs1Secondary);

    jsTest.log("Testing active connection with second shard down...");

    merizosConnActive.setSlaveOk();
    assert.neq(null, merizosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    assert.writeError(merizosConnActive.getCollection(collSharded.toString()).insert({_id: -11}));
    assert.writeError(merizosConnActive.getCollection(collSharded.toString()).insert({_id: 11}));
    assert.writeError(merizosConnActive.getCollection(collUnsharded.toString()).insert({_id: 11}));

    jsTest.log("Testing idle connection with second shard down...");

    assert.writeError(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: -12}));
    assert.writeError(merizosConnIdle.getCollection(collSharded.toString()).insert({_id: 12}));
    assert.writeError(merizosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 12}));

    merizosConnIdle.setSlaveOk();
    assert.neq(null, merizosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
    assert.neq(null, merizosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    jsTest.log("Testing new connections with second shard down...");

    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setSlaveOk();
    assert.neq(null, merizosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
    merizosConnNew = new Merizo(merizos.host);
    merizosConnNew.setSlaveOk();
    assert.neq(null, merizosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

    merizosConnNew = new Merizo(merizos.host);
    assert.writeError(merizosConnNew.getCollection(collSharded.toString()).insert({_id: -13}));
    merizosConnNew = new Merizo(merizos.host);
    assert.writeError(merizosConnNew.getCollection(collSharded.toString()).insert({_id: 13}));
    merizosConnNew = new Merizo(merizos.host);
    assert.writeError(merizosConnNew.getCollection(collUnsharded.toString()).insert({_id: 13}));

    gc();  // Clean up new connections

    st.stop();

})();
