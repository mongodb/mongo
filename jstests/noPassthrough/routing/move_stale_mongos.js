//
// Tests that stale mongoses can properly move chunks.
//

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 3,
    mongos: 2,
    other: {
        rsOptions: {
            setParameter: {
                // Reduce migrationRecipientPITHistoryToPreserveInSecs to 0 to allow chunks to be
                // migrated back and forth between shards.
                // There is no risk in doing so because `test.foo` collection is not receiving snapshot reads.
                migrationRecipientPITHistoryToPreserveInSecs: 0,
                // Set orphanCleanupDelaySecs to 0 to speed up this test.
                orphanCleanupDelaySecs: 0,
            },
        },
    },
});
let admin = st.s0.getDB("admin");
let testDb = "test";
let testNs = "test.foo";

assert.commandWorked(admin.runCommand({enableSharding: testDb, primaryShard: st.shard0.name}));
assert.commandWorked(admin.runCommand({shardCollection: testNs, key: {_id: 1}}));
let curShardIndex = 0;

for (let i = 0; i < 100; i += 10) {
    let shardIndex = i % 3;
    let toShard =
        shardIndex == 0 ? st.shard0.name : shardIndex == 1 ? st.shard1.name : st.shard2.name;
    let mongos = i % 2 == 0 ? st.s0 : st.s1;
    assert.commandWorked(
        mongos
            .getDB("admin")
            .runCommand({moveRange: testNs, min: {_id: i}, toShard: toShard, waitForDelete: true}),
    );

    // Ensure that other mongos sees the move
    st.configRS.awaitLastOpCommitted();
}

st.stop();
