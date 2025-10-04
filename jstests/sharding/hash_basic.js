import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let st = new ShardingTest({shards: 2, chunkSize: 1});

assert.commandWorked(st.s0.adminCommand({enableSharding: "test", primaryShard: st.shard1.shardName}));
assert.commandWorked(st.s0.adminCommand({shardCollection: "test.user", key: {x: "hashed"}}));

let configDB = st.s0.getDB("config");
let chunkCountBefore = findChunksUtil.countChunksForNs(configDB, "test.user");
assert.gt(chunkCountBefore, 1);

let testDB = st.s0.getDB("test");
for (let x = 0; x < 1000; x++) {
    testDB.user.insert({x: x});
}

let chunkDoc = findChunksUtil.findChunksByNs(configDB, "test.user").sort({min: 1}).next();
let min = chunkDoc.min;
let max = chunkDoc.max;

// Assumption: There are documents in the MinKey chunk, otherwise, splitVector will fail.
//
// Note: This chunk will have 267 documents if collection was presplit to 4.
let cmdRes = assert.commandWorked(
    st.s0.adminCommand({split: "test.user", bounds: [min, max]}),
    "Split on bounds failed for chunk [" + tojson(chunkDoc) + "]",
);

chunkDoc = findChunksUtil.findChunksByNs(configDB, "test.user").sort({min: 1}).skip(1).next();

let middle = NumberLong(chunkDoc.min.x + 1000000);
cmdRes = assert.commandWorked(
    st.s0.adminCommand({split: "test.user", middle: {x: middle}}),
    "Split failed with middle [" + middle + "]",
);

cmdRes = assert.commandWorked(st.s0.adminCommand({split: "test.user", find: {x: 7}}), "Split failed with find.");

let chunkList = findChunksUtil.findChunksByNs(configDB, "test.user").sort({min: 1}).toArray();
assert.eq(chunkCountBefore + 3, chunkList.length);

chunkList.forEach(function (chunkToMove) {
    let toShard = configDB.shards.findOne({_id: {$ne: chunkToMove.shard}})._id;

    print("Moving chunk " + chunkToMove._id + " from shard " + chunkToMove.shard + " to " + toShard + " ...");

    assert.commandWorked(
        st.s0.adminCommand({
            moveChunk: "test.user",
            bounds: [chunkToMove.min, chunkToMove.max],
            to: toShard,
            _waitForDelete: true,
        }),
    );
});

st.stop();
