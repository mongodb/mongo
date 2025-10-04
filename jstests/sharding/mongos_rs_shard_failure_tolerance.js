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

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// The following checks involve talking to shard primaries, as by the end of this test, one shard
// does not have a primary.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckMetadataConsistency = true;

// The routing table consistency check runs with 'snapshot' level readConcern. This readConcern
// level cannot be satisfied without a replica set primary, which we won't have because this test
// removes the replica set primary from a shard.
TestData.skipCheckRoutingTableConsistency = true;

let st = new ShardingTest({
    shards: 3,
    other: {
        rs: true,
        // Disables elections to avoid secondaries becoming primaries after stepdowns. The test
        // relies on specific topology changes done explicitly.
        rsOptions: {nodes: 2, settings: {electionTimeoutMillis: ReplSetTest.kForeverMillis}},
        // ShardingTest use a high config command timeout to avoid spurious failures but this test
        // may require a timeout to complete, so we restore the default value to avoid failures.
        mongosOptions: {setParameter: {defaultConfigCommandTimeoutMS: 30000}},
    },
});

let mongos = st.s0;
let admin = mongos.getDB("admin");

assert.commandWorked(admin.runCommand({setParameter: 1, traceExceptions: true}));

let collSharded = mongos.getCollection("fooSharded.barSharded");
let collUnsharded = mongos.getCollection("fooUnsharded.barUnsharded");

// Create the database for the unsharded collection
assert.commandWorked(
    admin.runCommand({enableSharding: collUnsharded.getDB().toString(), primaryShard: st.shard0.shardName}),
);
assert.commandWorked(collUnsharded.insert({some: "doc"}));
assert.commandWorked(collUnsharded.remove({}));

// Create the database for the sharded collection
assert.commandWorked(
    admin.runCommand({enableSharding: collSharded.getDB().toString(), primaryShard: st.shard0.shardName}),
);
assert.commandWorked(admin.runCommand({shardCollection: collSharded.toString(), key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: collSharded.toString(), middle: {_id: 0}}));
assert.commandWorked(admin.runCommand({moveChunk: collSharded.toString(), find: {_id: 0}, to: st.shard1.shardName}));

// Secondaries do not refresh their in-memory routing table until a request with a higher
// version is received, and refreshing requires communication with the primary to obtain the
// newest version. Read from the secondaries once before taking down primaries to ensure they
// have loaded the routing table into memory.
let mongosSetupConn = new Mongo(mongos.host);
mongosSetupConn.setReadPref("secondary");
assert(!mongosSetupConn.getCollection(collSharded.toString()).find({}).hasNext());

gc(); // Clean up connections

st.printShardingStatus();

//
// Setup is complete
//

jsTest.log("Inserting initial data...");

let mongosConnActive = new Mongo(mongos.host);
let mongosConnIdle = null;
let mongosConnNew = null;

let wc = {writeConcern: {w: 2, wtimeout: 60000}};

assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -1}, wc));
assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 1}, wc));
assert.commandWorked(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 1}, wc));

jsTest.log("Stopping primary of third shard...");

mongosConnIdle = new Mongo(mongos.host);

st.rs2.stop(st.rs2.getPrimary());

jsTest.log("Testing active connection with third primary down...");

assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -2}, wc));
assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 2}, wc));
assert.commandWorked(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 2}, wc));

jsTest.log("Testing idle connection with third primary down...");

assert.commandWorked(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -3}, wc));
assert.commandWorked(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 3}, wc));
assert.commandWorked(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 3}, wc));

assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with third primary down...");

mongosConnNew = new Mongo(mongos.host);
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = new Mongo(mongos.host);
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
mongosConnNew = new Mongo(mongos.host);
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnNew = new Mongo(mongos.host);
assert.commandWorked(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -4}, wc));
mongosConnNew = new Mongo(mongos.host);
assert.commandWorked(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 4}, wc));
mongosConnNew = new Mongo(mongos.host);
assert.commandWorked(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 4}, wc));

gc(); // Clean up new connections

jsTest.log("Stopping primary of second shard...");

mongosConnIdle = new Mongo(mongos.host);

// Need to save this node for later
let rs1Secondary = st.rs1.getSecondary();

st.rs1.stop(st.rs1.getPrimary());

jsTest.log("Testing active connection with second primary down...");

// Reads with read prefs
mongosConnActive.setSecondaryOk();
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));
mongosConnActive.setSecondaryOk(false);

mongosConnActive.setReadPref("primary");
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.throws(function () {
    mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1});
});
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

// Ensure read prefs override slaveOK
mongosConnActive.setSecondaryOk();
mongosConnActive.setReadPref("primary");
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.throws(function () {
    mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1});
});
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));
mongosConnActive.setSecondaryOk(false);

mongosConnActive.setReadPref("secondary");
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnActive.setReadPref("primaryPreferred");
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnActive.setReadPref("secondaryPreferred");
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnActive.setReadPref("nearest");
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

// Writes
assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -5}, wc));
assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 5}, wc));
assert.commandWorked(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 5}, wc));

jsTest.log("Testing idle connection with second primary down...");

// Writes
assert.commandWorked(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -6}, wc));
assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 6}, wc));
assert.commandWorked(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 6}, wc));

// Reads with read prefs
mongosConnIdle.setSecondaryOk();
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));
mongosConnIdle.setSecondaryOk(false);

mongosConnIdle.setReadPref("primary");
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.throws(function () {
    mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1});
});
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

// Ensure read prefs override slaveOK
mongosConnIdle.setSecondaryOk();
mongosConnIdle.setReadPref("primary");
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.throws(function () {
    mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1});
});
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));
mongosConnIdle.setSecondaryOk(false);

mongosConnIdle.setReadPref("secondary");
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnIdle.setReadPref("primaryPreferred");
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnIdle.setReadPref("secondaryPreferred");
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnIdle.setReadPref("nearest");
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with second primary down...");

// Reads with read prefs
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

gc(); // Clean up new connections incrementally to compensate for slow win32 machine.

mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("primary");
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("primary");
assert.throws(function () {
    mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1});
});
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("primary");
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

gc(); // Clean up new connections incrementally to compensate for slow win32 machine.

// Ensure read prefs override slaveok
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setSecondaryOk();
mongosConnNew.setReadPref("primary");
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setSecondaryOk();
mongosConnNew.setReadPref("primary");
assert.throws(function () {
    mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1});
});
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setSecondaryOk();
mongosConnNew.setReadPref("primary");
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

gc(); // Clean up new connections incrementally to compensate for slow win32 machine.

mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("secondary");
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("secondary");
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("secondary");
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

gc(); // Clean up new connections incrementally to compensate for slow win32 machine.

mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("primaryPreferred");
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("primaryPreferred");
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("primaryPreferred");
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

gc(); // Clean up new connections incrementally to compensate for slow win32 machine.

mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("secondaryPreferred");
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("secondaryPreferred");
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("secondaryPreferred");
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

gc(); // Clean up new connections incrementally to compensate for slow win32 machine.

mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("nearest");
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("nearest");
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setReadPref("nearest");
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

gc(); // Clean up new connections incrementally to compensate for slow win32 machine.

// Writes
mongosConnNew = new Mongo(mongos.host);
assert.commandWorked(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -7}, wc));
mongosConnNew = new Mongo(mongos.host);
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 7}, wc));
mongosConnNew = new Mongo(mongos.host);
assert.commandWorked(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 7}, wc));

gc(); // Clean up new connections

jsTest.log("Stopping primary of first shard...");

mongosConnIdle = new Mongo(mongos.host);

st.rs0.stop(st.rs0.getPrimary());

jsTest.log("Testing active connection with first primary down...");

mongosConnActive.setSecondaryOk();
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -8}));
assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 8}));
assert.writeError(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 8}));

jsTest.log("Testing idle connection with first primary down...");

assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -9}));
assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 9}));
assert.writeError(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 9}));

mongosConnIdle.setSecondaryOk();
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with first primary down...");

mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: 1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnNew = new Mongo(mongos.host);
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -10}));
mongosConnNew = new Mongo(mongos.host);
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 10}));
mongosConnNew = new Mongo(mongos.host);
assert.writeError(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 10}));

gc(); // Clean up new connections

jsTest.log("Stopping second shard...");

mongosConnIdle = new Mongo(mongos.host);

st.rs1.stop(rs1Secondary);

jsTest.log("Testing active connection with second shard down...");

mongosConnActive.setSecondaryOk();
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -11}));
assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 11}));
assert.writeError(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 11}));

jsTest.log("Testing idle connection with second shard down...");

assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -12}));
assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 12}));
assert.writeError(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 12}));

mongosConnIdle.setSecondaryOk();
assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections with second shard down...");

mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));
mongosConnNew = new Mongo(mongos.host);
mongosConnNew.setSecondaryOk();
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnNew = new Mongo(mongos.host);
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -13}));
mongosConnNew = new Mongo(mongos.host);
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 13}));
mongosConnNew = new Mongo(mongos.host);
assert.writeError(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 13}));

gc(); // Clean up new connections

st.stop();
