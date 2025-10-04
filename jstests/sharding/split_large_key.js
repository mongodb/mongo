// Test for splitting a chunk with a very large shard key value.
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

// Tests
//  - name: Name of test, used in collection name
//  - key: key to test
//  - keyFieldSize: size of each key field
let tests = [
    {name: "Key size small", key: {x: 1}, keyFieldSize: 100},
    {name: "Key size 512", key: {x: 1}, keyFieldSize: 512},
    {name: "Key size 2000", key: {x: 1}, keyFieldSize: 2000},
    {name: "Compound key size small", key: {x: 1, y: 1}, keyFieldSize: 100},
    {name: "Compound key size 512", key: {x: 1, y: 1}, keyFieldSize: 256},
    {name: "Compound key size 10000", key: {x: 1, y: 1}, keyFieldSize: 5000},
];

let st = new ShardingTest({shards: 1});
let configDB = st.s.getDB("config");

assert.commandWorked(configDB.adminCommand({enableSharding: "test"}));

tests.forEach(function (test) {
    let collName = "split_large_key_" + test.name;
    let midKey = {};
    let chunkKeys = {min: {}, max: {}};
    for (let k in test.key) {
        midKey[k] = "a".repeat(test.keyFieldSize);
        // min & max keys for each field in the index
        chunkKeys.min[k] = MinKey;
        chunkKeys.max[k] = MaxKey;
    }

    assert.commandWorked(configDB.adminCommand({shardCollection: "test." + collName, key: test.key}));

    let res = configDB.adminCommand({split: "test." + collName, middle: midKey});
    assert(res.ok, "Split: " + collName + " " + res.errmsg);

    assert.eq(2, findChunksUtil.findChunksByNs(configDB, "test." + collName).count(), "Chunks count split");

    st.s0.getCollection("test." + collName).drop();
});

st.stop();
