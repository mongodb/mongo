// Test hashed presplit with 1 shard.

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let st = new ShardingTest({shards: 1});
let testDB = st.getDB("test");

// create hashed shard key and enable sharding
testDB.adminCommand({enablesharding: "test"});
testDB.adminCommand({shardCollection: "test.collection", key: {a: "hashed"}});

// check the number of initial chunks.
assert.eq(
    1,
    findChunksUtil.countChunksForNs(st.getDB("config"), "test.collection"),
    "Using hashed shard key but failing to do correct presplitting",
);
st.stop();
