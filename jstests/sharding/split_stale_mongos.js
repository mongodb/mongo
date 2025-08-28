//
// Tests that stale mongoses can properly split chunks.
//

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2, mongos: 2});
let admin = st.s0.getDB("admin");
let testDb = "test";
let testNs = "test.foo";

assert.commandWorked(admin.runCommand({enableSharding: testDb}));
assert.commandWorked(admin.runCommand({shardCollection: testNs, key: {_id: 1}}));

for (let i = 0; i < 100; i += 10) {
    assert.commandWorked(st.s0.getDB("admin").runCommand({split: testNs, middle: {_id: i}}));
    st.configRS.awaitLastOpCommitted(); // Ensure that other mongos sees the previous split
    assert.commandWorked(st.s1.getDB("admin").runCommand({split: testNs, middle: {_id: i + 5}}));
    st.configRS.awaitLastOpCommitted(); // Ensure that other mongos sees the previous split
}

st.stop();
