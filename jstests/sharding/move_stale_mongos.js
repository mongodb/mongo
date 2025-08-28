//
// Tests that stale mongoses can properly move chunks.
//

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2, mongos: 2});
let admin = st.s0.getDB("admin");
let testDb = "test";
let testNs = "test.foo";

assert.commandWorked(admin.runCommand({enableSharding: testDb, primaryShard: st.shard0.name}));
assert.commandWorked(admin.runCommand({shardCollection: testNs, key: {_id: 1}}));
let curShardIndex = 0;

for (let i = 0; i < 100; i += 10) {
    assert.commandWorked(st.s0.getDB("admin").runCommand({split: testNs, middle: {_id: i}}));
    st.configRS.awaitLastOpCommitted(); // Ensure that other mongos sees the split
    let nextShardIndex = (curShardIndex + 1) % 2;
    let toShard = nextShardIndex == 0 ? st.shard0.name : st.shard1.name;
    assert.commandWorked(
        st.s1.getDB("admin").runCommand({moveChunk: testNs, find: {_id: i + 5}, to: toShard, _waitForDelete: true}),
    );
    curShardIndex = nextShardIndex;
    st.configRS.awaitLastOpCommitted(); // Ensure that other mongos sees the move
}

st.stop();
