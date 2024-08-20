// Test hashed presplit with 1 shard.

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

var st = new ShardingTest({shards: 1});
var testDB = st.getDB('test');

// create hashed shard key and enable sharding
testDB.adminCommand({enablesharding: "test"});
testDB.adminCommand({shardCollection: "test.collection", key: {a: "hashed"}});

let expectedChunkCount = 1;
// TODO SERVER-81884: update once 8.0 becomes last LTS.
if (!FeatureFlagUtil.isPresentAndEnabled(testDB,
                                         "OneChunkPerShardEmptyCollectionWithHashedShardKey")) {
    expectedChunkCount = 2;
}
// check the number of initial chunks.
assert.eq(expectedChunkCount,
          findChunksUtil.countChunksForNs(st.getDB('config'), "test.collection"),
          'Using hashed shard key but failing to do correct presplitting');
st.stop();
