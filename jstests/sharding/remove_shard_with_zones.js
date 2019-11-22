/**
 * Test that removeShard disallows removing shards whose chunks cannot be drained
 * to other shards.
 */
(function() {
'use strict';

let st = new ShardingTest({shards: 3});
let dbName = "test";
let collName = "user";
let ns = dbName + "." + collName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "zoneA"}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "zoneB"}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: "zoneC"}));

// Can remove the only shard for a zone if that zone does not have any chunk ranges.
assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));

// Cannot remove the only shard for a zone if that zone has chunk ranges associated with it.
assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: ns, min: {x: MinKey}, max: {x: MaxKey}, zone: "zoneC"}));
assert.commandFailedWithCode(st.s.adminCommand({removeShard: st.shard2.shardName}),
                             ErrorCodes.ZoneStillInUse);

// Can remove a shard is if it is not the only shard for any zone.
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "zoneC"}));
assert.commandWorked(st.s.adminCommand({removeShard: st.shard2.shardName}));

st.stop();
})();
