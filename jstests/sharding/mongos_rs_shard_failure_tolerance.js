//
// Tests mongos's failure tolerance for replica set shards and read preference queries
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

var options = {separateConfig : true,
               rs : true,
               rsOptions : { nodes : 2 }};

var st = new ShardingTest({shards : 3, mongos : 1, other : options});
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB( "admin" );
var shards = mongos.getDB( "config" ).shards.find().toArray();

assert.commandWorked( admin.runCommand({ setParameter : 1, traceExceptions : true }) );

var collSharded = mongos.getCollection( "fooSharded.barSharded" );
var collUnsharded = mongos.getCollection( "fooUnsharded.barUnsharded" );

// Create the unsharded database
assert.writeOK(collUnsharded.insert({ some : "doc" }));
assert.writeOK(collUnsharded.remove({}));
printjson( admin.runCommand({ movePrimary : collUnsharded.getDB().toString(),
                              to : shards[0]._id }) );

// Create the sharded database
assert.commandWorked( admin.runCommand({ enableSharding : collSharded.getDB().toString() }) );
printjson( admin.runCommand({ movePrimary : collSharded.getDB().toString(), to : shards[0]._id }) );
assert.commandWorked( admin.runCommand({ shardCollection : collSharded.toString(),
                                         key : { _id : 1 } }) );
assert.commandWorked( admin.runCommand({ split : collSharded.toString(), middle : { _id : 0 } }) );
assert.commandWorked( admin.runCommand({ moveChunk : collSharded.toString(),
                                         find : { _id : 0 },
                                         to : shards[1]._id }) );

st.printShardingStatus();

//
// Setup is complete
//

jsTest.log("Inserting initial data...");

var mongosConnActive = new Mongo( mongos.host );
var mongosConnIdle = null;
var mongosConnNew = null;

assert.writeOK(mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : -1 }));
assert.writeOK(mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : 1 }));
assert.writeOK(mongosConnActive.getCollection( collUnsharded.toString() ).insert({ _id : 1 }));

jsTest.log("Stopping primary of third shard...");

mongosConnIdle = new Mongo( mongos.host );

st.rs2.stop(st.rs2.getPrimary(), true /*wait for stop*/ );

jsTest.log("Testing active connection with third primary down...");

assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

assert.writeOK(mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : -2 }));
assert.writeOK(mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : 2 }));
assert.writeOK(mongosConnActive.getCollection( collUnsharded.toString() ).insert({ _id : 2 }));

jsTest.log("Testing idle connection with third primary down...");

assert.writeOK(mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : -3 }));
assert.writeOK(mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : 3 }));
assert.writeOK(mongosConnIdle.getCollection( collUnsharded.toString() ).insert({ _id : 3 }));

assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : 1 }) );
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

jsTest.log("Testing new connections with third primary down...");

mongosConnNew = new Mongo( mongos.host );
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
mongosConnNew = new Mongo( mongos.host );
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : 1 }) );
mongosConnNew = new Mongo( mongos.host );
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

mongosConnNew = new Mongo( mongos.host );
assert.writeOK(mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : -4 }));
mongosConnNew = new Mongo( mongos.host );
assert.writeOK(mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : 4 }));
mongosConnNew = new Mongo( mongos.host );
assert.writeOK(mongosConnNew.getCollection( collUnsharded.toString() ).insert({ _id : 4 }));

gc(); // Clean up new connections

jsTest.log("Stopping primary of second shard...");

mongosConnIdle = new Mongo( mongos.host );

// Need to save this node for later
var rs1Secondary = st.rs1.getSecondary();

st.rs1.stop(st.rs1.getPrimary(), true /* wait for stop */);

jsTest.log("Testing active connection with second primary down...");

// Reads with read prefs
mongosConnActive.setSlaveOk();
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));
mongosConnActive.setSlaveOk(false);

mongosConnActive.setReadPref("primary");
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.throws(function() {
    mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : 1 });
});
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

//Ensure read prefs override slaveOK
mongosConnActive.setSlaveOk();
mongosConnActive.setReadPref("primary");
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.throws(function() {
    mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : 1 });
});
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));
mongosConnActive.setSlaveOk(false);

mongosConnActive.setReadPref("secondary");
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnActive.setReadPref("primaryPreferred");
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnActive.setReadPref("secondaryPreferred");
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnActive.setReadPref("nearest");
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

// Writes
assert.writeOK(mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : -5 }));
assert.writeError(mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : 5 }));
assert.writeOK(mongosConnActive.getCollection( collUnsharded.toString() ).insert({ _id : 5 }));

jsTest.log("Testing idle connection with second primary down...");

// Writes
assert.writeOK(mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : -6 }));
assert.writeError(mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : 6 }));
assert.writeOK(mongosConnIdle.getCollection( collUnsharded.toString() ).insert({ _id : 6 }));

// Reads with read prefs
mongosConnIdle.setSlaveOk();
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : 1 }) );
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );
mongosConnIdle.setSlaveOk(false);

mongosConnIdle.setReadPref("primary");
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.throws(function() {
    mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : 1 });
});
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

// Ensure read prefs override slaveOK
mongosConnIdle.setSlaveOk();
mongosConnIdle.setReadPref("primary");
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.throws(function() {
    mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : 1 });
});
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));
mongosConnIdle.setSlaveOk(false);

mongosConnIdle.setReadPref("secondary");
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnIdle.setReadPref("primaryPreferred");
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnIdle.setReadPref("secondaryPreferred");
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnIdle.setReadPref("nearest");
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

jsTest.log("Testing new connections with second primary down...");

// Reads with read prefs
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : 1 }) );
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("primary");
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("primary");
assert.throws(function() {
    mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : 1 });
});
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("primary");
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

// Ensure read prefs override slaveok
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
mongosConnNew.setReadPref("primary");
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
mongosConnNew.setReadPref("primary");
assert.throws(function() {
    mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : 1 });
});
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
mongosConnNew.setReadPref("primary");
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("secondary");
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("secondary");
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("secondary");
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("primaryPreferred");
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("primaryPreferred");
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("primaryPreferred");
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("secondaryPreferred");
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("secondaryPreferred");
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("secondaryPreferred");
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("nearest");
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("nearest");
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setReadPref("nearest");
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

// Writes
mongosConnNew = new Mongo( mongos.host );
assert.writeOK(mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : -7 }));
mongosConnNew = new Mongo( mongos.host );
assert.writeError(mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : 7 }));
mongosConnNew = new Mongo( mongos.host );
assert.writeOK(mongosConnNew.getCollection( collUnsharded.toString() ).insert({ _id : 7 }));

gc(); // Clean up new connections

jsTest.log("Stopping primary of first shard...");

mongosConnIdle = new Mongo( mongos.host );

st.rs0.stop(st.rs0.getPrimary(), true /*wait for stop*/ );

jsTest.log("Testing active connection with first primary down...");

mongosConnActive.setSlaveOk();
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : 1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

assert.writeError(mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : -8 }));
assert.writeError(mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : 8 }));
assert.writeError(mongosConnActive.getCollection( collUnsharded.toString() ).insert({ _id : 8 }));

jsTest.log("Testing idle connection with first primary down...");

assert.writeError(mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : -9 }));
assert.writeError(mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : 9 }));
assert.writeError(mongosConnIdle.getCollection( collUnsharded.toString() ).insert({ _id : 9 }));

mongosConnIdle.setSlaveOk();
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : 1 }) );
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

jsTest.log("Testing new connections with first primary down...");

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : 1 }) );
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

mongosConnNew = new Mongo( mongos.host );
assert.writeError(mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : -10 }));
mongosConnNew = new Mongo( mongos.host );
assert.writeError(mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : 10 }));
mongosConnNew = new Mongo( mongos.host );
assert.writeError(mongosConnNew.getCollection( collUnsharded.toString() ).insert({ _id : 10 }));

gc(); // Clean up new connections

jsTest.log("Stopping second shard...");

mongosConnIdle = new Mongo( mongos.host );

st.rs1.stop(rs1Secondary, true /* wait for stop */);

jsTest.log("Testing active connection with second shard down...");

mongosConnActive.setSlaveOk();
assert.neq(null, mongosConnActive.getCollection( collSharded.toString() ).findOne({ _id : -1 }));
assert.neq(null, mongosConnActive.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }));

assert.writeError(mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : -11 }));
assert.writeError(mongosConnActive.getCollection( collSharded.toString() ).insert({ _id : 11 }));
assert.writeError(mongosConnActive.getCollection( collUnsharded.toString() ).insert({ _id : 11 }));

jsTest.log("Testing idle connection with second shard down...");

assert.writeError(mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : -12 }));
assert.writeError(mongosConnIdle.getCollection( collSharded.toString() ).insert({ _id : 12 }));
assert.writeError(mongosConnIdle.getCollection( collUnsharded.toString() ).insert({ _id : 12 }));

mongosConnIdle.setSlaveOk();
assert.neq(null, mongosConnIdle.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
assert.neq(null, mongosConnIdle.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

jsTest.log("Testing new connections with second shard down...");

mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collSharded.toString() ).findOne({ _id : -1 }) );
mongosConnNew = new Mongo( mongos.host );
mongosConnNew.setSlaveOk();
assert.neq(null, mongosConnNew.getCollection( collUnsharded.toString() ).findOne({ _id : 1 }) );

mongosConnNew = new Mongo( mongos.host );
assert.writeError(mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : -13 }));
mongosConnNew = new Mongo( mongos.host );
assert.writeError(mongosConnNew.getCollection( collSharded.toString() ).insert({ _id : 13 }));
mongosConnNew = new Mongo( mongos.host );
assert.writeError(mongosConnNew.getCollection( collUnsharded.toString() ).insert({ _id : 13 }));

gc(); // Clean up new connections

jsTest.log("DONE!");
st.stop();

