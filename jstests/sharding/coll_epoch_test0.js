// Tests whether a split and a migrate in a sharded cluster preserve the epoch\

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2, mongos: 1});
// Balancer is by default stopped, thus it will not interfere

let config = st.s.getDB("config");
let admin = st.s.getDB("admin");
const kDbName = "foo";

// First enable sharding
admin.runCommand({enableSharding: kDbName, primaryShard: st.shard1.shardName});
let coll = st.s.getCollection(kDbName + ".bar");
admin.runCommand({shardCollection: coll + "", key: {_id: 1}});

let primary = config.databases.find({_id: coll.getDB() + ""}).primary;
let notPrimary = null;
config.shards.find().forEach(function (doc) {
    if (doc._id != primary) notPrimary = doc._id;
});

let originalUuid = config.collections.findOne({_id: coll + ""}).uuid;
let originalEpoch = config.collections.findOne({_id: coll + ""}).lastmodEpoch;

// Now do a split
printjson(admin.runCommand({split: coll + "", middle: {_id: 0}}));

assert.eq(originalUuid, config.collections.findOne({_id: coll + ""}).uuid);
assert.eq(originalEpoch, config.collections.findOne({_id: coll + ""}).lastmodEpoch);

// Now do a migrate
printjson(admin.runCommand({moveChunk: coll + "", find: {_id: 0}, to: notPrimary}));

assert.eq(originalUuid, config.collections.findOne({_id: coll + ""}).uuid);
assert.eq(originalEpoch, config.collections.findOne({_id: coll + ""}).lastmodEpoch);

st.stop();
