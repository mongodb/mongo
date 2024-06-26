/**
 * Verifies that secondaries clear their filtering metadata whenever a collection is created.
 *
 * TODO (SERVER-91143): Move and adapt this test to the `core_sharding_passthrough` suite once the
 * infrastructure is ready to support at least two routers.
 */

const st = new ShardingTest({mongos: 2, shards: 2, rs: {nodes: 2}});

const s0 = st.s0;
const s1 = st.s1;

const dbName = "MyDb";
const collName = "MyColl";
const collNs = dbName + "." + collName;

assert.commandWorked(s0.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(s0.adminCommand({shardCollection: collNs, key: {x: 1}}));
assert.commandWorked(s0.adminCommand({split: collNs, middle: {x: 0}}));
assert.commandWorked(s0.adminCommand({moveChunk: collNs, find: {x: 0}, to: st.shard1.shardName}));

// router0: SV1
// router1: Empty
// shard0: primary=SV1, secondary=UNKNOWN
// shard1: primary=SV2, secondary=UNKNOWN

assert.commandWorked(s0.getDB(dbName).MyColl.insert({x: -100}));
assert.commandWorked(s0.getDB(dbName).MyColl.insert({x: 100}));
assert.commandWorked(s1.getDB(dbName).MyColl.insert({x: -1000}));
assert.commandWorked(s1.getDB(dbName).MyColl.insert({x: 1000}));

// router0: SV1
// router1: SV1
// shard0: primary=SV1, secondary=UNKNOWN
// shard1: primary=SV2, secondary=UNKNOWN

s0.getDB(dbName).MyColl.drop();

// router0: Empty
// router1: SV1 (not aware of drop)
// shard0: primary=UNKNOWN, secondary=UNKNOWN
// shard1: primary=UNKNOWN, secondary=UNKNOWN

// router1 will target shard1, then shard0.
s1.getDB(dbName).MyColl.find({x: 0}).readPref('secondary').toArray();

// router0: Empty
// router1: Empty
// shard0: primary=UNSHARDED, secondary=UNSHARDED
// shard1: primary=UNSHARDED, secondary=UNSHARDED

assert.commandWorked(s0.adminCommand({shardCollection: collNs, key: {y: 1}}));
assert.commandWorked(s0.getDB(dbName).MyColl.insert({y: 42}));

// router0: SV1
// router1: Empty
// shard0: primary=SV1, secondary=UNKNOWN
// shard1: primary=UNSHARDED, secondary=UNSHARDED

// This should reset shard1's shard version to be UNKNOWN on all nodes.
assert.commandWorked(s0.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

// router0: SV1
// router1: Empty
// shard0: primary=SV1, secondary=UNKNOWN
// shard1: primary=UNKNOWN, secondary=UNKNOWN

// If this were to equal 0 it would mean that s1 sent an UNSHARDED version to a stale secondary on
// the new dbPrimary shard1. Being 1 means we're doing the correct thing here.
assert.eq(s1.getDB(dbName).MyColl.find({}).readPref('secondary').itcount(), 1);
assert.eq(s0.getDB(dbName).MyColl.find({}).itcount(), 1);

st.stop();
