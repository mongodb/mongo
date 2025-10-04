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
// The following checks involve talking to shards, but this test shuts down shards.
import {ShardingTest} from "jstests/libs/shardingtest.js";

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckShardFilteringMetadata = true;

let st = new ShardingTest({shards: 3, mongos: 1});

let admin = st.s0.getDB("admin");

const dbCollSharded = "fooSharded";
const dbCollUnsharded = "fooUnsharded";
assert.commandWorked(admin.runCommand({enableSharding: dbCollSharded, primaryShard: st.shard0.shardName}));
assert.commandWorked(admin.runCommand({enableSharding: dbCollUnsharded, primaryShard: st.shard0.shardName}));
let collSharded = st.s0.getCollection(dbCollSharded + ".barSharded");
let collUnsharded = st.s0.getCollection(dbCollUnsharded + ".barUnsharded");

assert.commandWorked(admin.runCommand({shardCollection: collSharded.toString(), key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: collSharded.toString(), middle: {_id: 0}}));
assert.commandWorked(admin.runCommand({moveChunk: collSharded.toString(), find: {_id: 0}, to: st.shard1.shardName}));

// Create the unsharded database
assert.commandWorked(collUnsharded.insert({some: "doc"}));
assert.commandWorked(collUnsharded.remove({}));

//
// Setup is complete
//

jsTest.log("Inserting initial data...");

let mongosConnActive = new Mongo(st.s0.host);
let mongosConnIdle = null;
let mongosConnNew = null;

assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -1}));
assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 1}));
assert.commandWorked(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 1}));

jsTest.log("Stopping third shard...");

mongosConnIdle = new Mongo(st.s0.host);

st.rs2.stopSet();

jsTest.log("Testing active connection...");

assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: 1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -2}));
assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 2}));
assert.commandWorked(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 2}));

jsTest.log("Testing idle connection...");

assert.commandWorked(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -3}));
assert.commandWorked(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 3}));
assert.commandWorked(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 3}));

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
assert.commandWorked(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -4}));
mongosConnNew = new Mongo(st.s0.host);
assert.commandWorked(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 4}));
mongosConnNew = new Mongo(st.s0.host);
assert.commandWorked(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 4}));

gc(); // Clean up new connections

jsTest.log("Stopping second shard...");

mongosConnIdle = new Mongo(st.s0.host);

st.rs1.stopSet();
jsTest.log("Testing active connection...");

assert.neq(null, mongosConnActive.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnActive.getCollection(collUnsharded.toString()).findOne({_id: 1}));

assert.commandWorked(mongosConnActive.getCollection(collSharded.toString()).insert({_id: -5}));

assert.writeError(mongosConnActive.getCollection(collSharded.toString()).insert({_id: 5}));
assert.commandWorked(mongosConnActive.getCollection(collUnsharded.toString()).insert({_id: 5}));

jsTest.log("Testing idle connection...");

assert.commandWorked(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: -6}));
assert.writeError(mongosConnIdle.getCollection(collSharded.toString()).insert({_id: 6}));
assert.commandWorked(mongosConnIdle.getCollection(collUnsharded.toString()).insert({_id: 6}));

assert.neq(null, mongosConnIdle.getCollection(collSharded.toString()).findOne({_id: -1}));
assert.neq(null, mongosConnIdle.getCollection(collUnsharded.toString()).findOne({_id: 1}));

jsTest.log("Testing new connections...");

mongosConnNew = new Mongo(st.s0.host);
assert.neq(null, mongosConnNew.getCollection(collSharded.toString()).findOne({_id: -1}));

mongosConnNew = new Mongo(st.s0.host);
assert.neq(null, mongosConnNew.getCollection(collUnsharded.toString()).findOne({_id: 1}));

mongosConnNew = new Mongo(st.s0.host);
assert.commandWorked(mongosConnNew.getCollection(collSharded.toString()).insert({_id: -7}));

mongosConnNew = new Mongo(st.s0.host);
assert.writeError(mongosConnNew.getCollection(collSharded.toString()).insert({_id: 7}));

mongosConnNew = new Mongo(st.s0.host);
assert.commandWorked(mongosConnNew.getCollection(collUnsharded.toString()).insert({_id: 7}));

st.stop();
