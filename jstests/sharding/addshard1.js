/**
 * @tags: [
 *   # This test is incompatible with 'config shard' as it creates a cluster with 0 shards in order
 *   # to be able to add shard with data on it (which is only allowed on the first shard).
 *   config_shard_incompatible,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {moveDatabaseAndUnshardedColls} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";

let s = new ShardingTest({name: "add_shard1", shards: 0, useHostname: false});

// Create a shard and add a database; for the first shard we allow data on the replica set
let rs1 = new ReplSetTest({name: "addshard1-1", host: "localhost", nodes: 1});
rs1.startSet({shardsvr: ""});
rs1.initiate();

let db1 = rs1.getPrimary().getDB("testDB");

let numObjs = 3;
for (let i = 0; i < numObjs; i++) {
    assert.commandWorked(db1.foo.save({a: i}));
}

let configDB = s.s.getDB("config");
assert.eq(null, configDB.databases.findOne({_id: "testDB"}));

let newShard = "myShard";
assert.commandWorked(s.admin.runCommand({addShard: rs1.getURL(), name: newShard}));

assert.neq(null, configDB.databases.findOne({_id: "testDB"}));

let newShardDoc = configDB.shards.findOne({_id: newShard});
assert(newShardDoc.topologyTime instanceof Timestamp);

// maxSize field is no longer supported
let newShardMaxSize = "myShardMaxSize";
assert.commandFailedWithCode(
    s.admin.runCommand({addShard: rs1.getURL(), name: newShardMaxSize, maxSize: 1024}),
    ErrorCodes.InvalidOptions,
);

// a second shard with existing data is rejected
let rs2 = new ReplSetTest({name: "addshard1-2", nodes: 1});
rs2.startSet({shardsvr: ""});
rs2.initiate();

let db2 = rs2.getPrimary().getDB("otherDB");
assert.commandWorked(db2.foo.save({a: 1}));

s.config.databases.find().forEach(printjson);

let rejectedShard = "rejectedShard";
assert(!s.admin.runCommand({addShard: rs2.getURL(), name: rejectedShard}).ok, "accepted mongod with duplicate db");

// Check that all collection that were local to the mongod's are accessible through the mongos
let sdb1 = s.getDB("testDB");
assert.eq(numObjs, sdb1.foo.count(), "wrong count for database that existed before addshard");

let sdb2 = s.getDB("otherDB");
assert.eq(0, sdb2.foo.count(), "database of rejected shard appears through mongos");

// make sure we can move a DB from the original mongod to a previoulsy existing shard
assert.eq(s.normalize(s.config.databases.findOne({_id: "testDB"}).primary), newShard, "DB primary is wrong");

let rs3 = new ReplSetTest({name: "addshard1-3", host: "localhost", nodes: 1});
rs3.startSet({shardsvr: ""});
rs3.initiate();
assert.commandWorked(s.admin.runCommand({addShard: rs3.getURL()}));

let origShard = s.getNonPrimaries("testDB")[0];
moveDatabaseAndUnshardedColls(s.s.getDB("testDB"), origShard);

assert.eq(s.normalize(s.config.databases.findOne({_id: "testDB"}).primary), origShard, "DB primary didn't move");
assert.eq(numObjs, sdb1.foo.count(), "wrong count after moving datbase that existed before addshard");

// make sure we can shard the original collections
sdb1.foo.createIndex({a: 1}, {unique: true}); // can't shard populated collection without an index
s.adminCommand({enablesharding: "testDB"});
s.adminCommand({shardcollection: "testDB.foo", key: {a: 1}});
s.adminCommand({split: "testDB.foo", middle: {a: Math.floor(numObjs / 2)}});
assert.eq(
    2,
    findChunksUtil.countChunksForNs(s.config, "testDB.foo"),
    "wrong chunk number after splitting collection that existed before",
);
assert.eq(numObjs, sdb1.foo.count(), "wrong count after splitting collection that existed before");

s.stop();
rs1.stopSet();
rs2.stopSet();
rs3.stopSet();
