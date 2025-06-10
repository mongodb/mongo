/**
 * Test that an unsuccessful addShard will preserve the UserWriteBlocking state of the replica set
 * being added to the cluster.
 *
 * @tags: [
 *   requires_persistence,
 *   multiversion_incompatible,
 *   requires_fcv_82,
 * ]
 */

const db = (replicaset) => {
    return replicaset.getPrimary().getDB("testDB");
};

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Create the sharded cluster with only one shard
var st = new ShardingTest({name: "st", shards: 1});

// Create a single node replica set and add a database
jsTest.log("Creating a single node replica set and adding data to it");
var rs0 = new ReplSetTest({name: "rs0", nodes: 1});
rs0.startSet();
rs0.initiate();
assert.commandWorked(db(rs0).coll.insertOne({a: 1}));

// Set user write blocking
jsTest.log("Setting the user write blocking should return OK");
assert.commandWorked(rs0.getPrimary().adminCommand({setUserWriteBlockMode: 1, global: true}));

// Test that writing to the replica set again fails
jsTest.log("Testing a write on the replica set should fail");
try {
    db(rs0).coll.insertOne({b: 1});
} catch (error) {
    assert.commandFailedWithCode(error, ErrorCodes.UserWritesBlocked);
}

jsTest.log("Adding the replica set to the sharded cluster should fail");
// Restart the replica set in shardsvr mode
rs0.restart(0, {shardsvr: ""});

// Test that adding the replica set to the cluster fails since the replica set has data (BYOD no
// longer supported)
assert.commandFailedWithCode(st.s.adminCommand({addShard: rs0.getURL()}),
                             ErrorCodes.IllegalOperation);

// Test that writing to the replica set again fails
jsTest.log("Testing a write on the replica set should fail again");
try {
    db(rs0).coll.insertOne({c: 1});
} catch (error) {
    assert.commandFailedWithCode(error, ErrorCodes.UserWritesBlocked);
}

// Create a single node replica set and add a database
jsTest.log("Creating a single node replica set and adding data to it");
var rs1 = new ReplSetTest({name: "rs1", nodes: 1});
rs1.startSet();
rs1.initiate();
assert.commandWorked(db(rs1).coll.insertOne({a: 1}));

// Set user write blocking
jsTest.log("Setting the user write blocking should return OK");
assert.commandWorked(rs1.getPrimary().adminCommand({setUserWriteBlockMode: 1, global: false}));

jsTest.log("Adding the replica set to the sharded cluster should fail");
// Restart the replica set in shardsvr mode
rs1.restart(0, {shardsvr: ""});

// Test that adding the replica set to the cluster fails since the replica set has data (BYOD no
// longer supported)
assert.commandFailedWithCode(st.s.adminCommand({addShard: rs1.getURL()}),
                             ErrorCodes.IllegalOperation);

// Test that writing to the replica set again works
jsTest.log("Testing a write on the replica set should work");
assert.commandWorked(db(rs1).coll.insertOne({b: 1}));

// Stop sharded cluster and replica set
st.stop();
rs0.stopSet();
rs1.stopSet();
