// Tests that adding an equivalent shard multiple times returns success.
(function() {
'use strict';

const st = new ShardingTest({name: "add_shard_idempotent", shards: 1});

jsTestLog("Testing adding a replica set shard multiple times");
const shard2 = new ReplSetTest({name: 'rsShard', nodes: 3});
shard2.startSet({shardsvr: ""});
shard2.initiate();
shard2.getPrimary();  // Wait for there to be a primary
const shard2SeedList1 = shard2.name + "/" + shard2.nodes[0].host;
const shard2SeedList2 = shard2.name + "/" + shard2.nodes[2].host;

assert.commandWorked(st.admin.runCommand({addshard: shard2SeedList1, name: "newShard2"}));

// Running the identical addShard command should succeed.
assert.commandWorked(st.admin.runCommand({addshard: shard2SeedList1, name: "newShard2"}));

// We can only compare replica sets by their set name, so calling addShard with a different
// seed list should still be considered a successful no-op.
assert.commandWorked(st.admin.runCommand({addshard: shard2SeedList2, name: "newShard2"}));

// Verify that the config.shards collection looks right.
const shards = st.s.getDB('config').shards.find().toArray();
assert.eq(2, shards.length);
let shard1TopologyTime, shard2TopologyTime;
for (let i = 0; i < shards.length; i++) {
    let shard = shards[i];
    if (shard._id != 'newShard2') {
        assert(shard.topologyTime instanceof Timestamp);
        shard1TopologyTime = shard.topologyTime;
    } else {
        assert.eq('newShard2', shard._id);
        assert.eq(shard2.getURL(), shard.host);
        assert(shard.topologyTime instanceof Timestamp);
        shard2TopologyTime = shard.topologyTime;
    }
}
assert.gt(shard2TopologyTime, shard1TopologyTime);
shard2.stopSet();
st.stop();
})();
