// Tests that executing splitChunk directly against a shard, with an invalid split point will not
// corrupt the chunks metadata
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 1});

let testDB = st.s.getDB("TestSplitDB");
assert.commandWorked(testDB.adminCommand({enableSharding: "TestSplitDB", primaryShard: st.shard0.shardName}));

assert.commandWorked(testDB.adminCommand({shardCollection: "TestSplitDB.Coll", key: {x: 1}}));
assert.commandWorked(testDB.adminCommand({split: "TestSplitDB.Coll", middle: {x: 0}}));

let chunksBefore = st.s.getDB("config").chunks.find().toArray();

// Try to do a split with invalid parameters through mongod
let callSplit = function (db, minKey, maxKey, splitPoints) {
    let res = assert.commandWorked(st.s.adminCommand({getShardVersion: "TestSplitDB.Coll"}));
    return db.runCommand({
        splitChunk: "TestSplitDB.Coll",
        from: st.shard0.shardName,
        min: minKey,
        max: maxKey,
        keyPattern: {x: 1},
        splitKeys: splitPoints,
        epoch: res.versionEpoch,
    });
};

assert.commandFailedWithCode(
    callSplit(st.rs0.getPrimary().getDB("admin"), {x: MinKey}, {x: 0}, [{x: 2}]),
    ErrorCodes.InvalidOptions,
);

let chunksAfter = st.s.getDB("config").chunks.find().toArray();
assert.eq(chunksBefore, chunksAfter, "Split chunks failed, but the chunks were updated in the config database");

st.stop();
