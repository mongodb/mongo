/**
 * Test that removeShard disallows removing shards whose chunks cannot be drained
 * to other shards.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

let st = new ShardingTest({shards: 3, other: {enableBalancer: true}});
let dbName = "test";
let collName = "user";
let ns = dbName + "." + collName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "zoneA"}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "zoneB"}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: "zoneC"}));

// Can remove the only shard for a zone if that zone does not have any chunk ranges.
removeShard(st, st.shard1.shardName);

// Cannot remove the only shard for a zone if that zone has chunk ranges associated with it.
assert.commandWorked(st.s.adminCommand({updateZoneKeyRange: ns, min: {x: MinKey}, max: {x: MaxKey}, zone: "zoneC"}));
assert.commandFailedWithCode(st.s.adminCommand({removeShard: st.shard2.shardName}), ErrorCodes.ZoneStillInUse);

// Can remove a shard is if it is not the only shard for any zone.
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "zoneC"}));
removeShard(st, st.shard2.shardName);

st.stop();
