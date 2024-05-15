/**
 * Verifies that secondaries clear their filtering metadata whenever we create a new collection.
 * This avoid having stale information present on the secondaries.
 */

const st = new ShardingTest({mongos: 2, shards: 2, rs: {nodes: 2}});

const s0 = st.s0;
const s1 = st.s1;
const dbName = "MyDb";
const collName = "MyColl";
const fullName = dbName + "." + collName;
assert.commandWorked(s0.adminCommand({enablesharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(s0.adminCommand({shardCollection: fullName, key: {x: 1}}));

assert.commandWorked(st.splitAt(fullName, {x: 0}));
assert.commandWorked(s0.adminCommand({moveChunk: fullName, find: {x: 0}, to: st.shard1.shardName}));

/*
 *   shard0: SV1 - [-inf, 0)
 *   shard1: SV2 - [0, +inf]
 */

assert.commandWorked(s0.getDB(dbName).MyColl.insert({x: -100}));
assert.commandWorked(s0.getDB(dbName).MyColl.insert({x: 100}));

assert.commandWorked(s1.getDB(dbName).MyColl.insert({x: -1000}));
assert.commandWorked(s1.getDB(dbName).MyColl.insert({x: 1000}));

// s0 and s1 have cached the routing info of MyColl.
s0.getDB(dbName).MyColl.drop();

/*
 *  shard0: UNKNOWN
 *  shard1: UNKNOWN
 *  s0: empty
 *  s1: SHARDED - Not aware of the drop
 */

// s1 will target shard1
s1.getDB(dbName).MyColl.find({x: 0}).readPref('secondary').toArray();

/*
 *   shard0: UNKNOWN
 *   shard1:
 *       Primary: UNKNWON
 *       Secondary: UNSHARDED
 *   s0: empty
 *   s1: UNSHARDED (i.e. not present in config.collections)
 */

assert.commandWorked(s0.adminCommand({shardCollection: fullName, key: {y: 1}}));
assert.commandWorked(s0.getDB(dbName).MyColl.insert({y: 42}));

/*
 *  shard0: SV1 [-inf, +inf]
 *  shard1:
 *      Primary: UNKNWON
 *      Secondary: UNSHARDED
 *  s0: SHARDED - SV1
 *  s1: UNSHARDED (i.e. not present in config.collections)
 */

// This should reset shard1's shard version to be UNKNOWN on all nodes.
assert.commandWorked(s0.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

/*
 *  shard0: SV1 [-inf, +inf]
 *  shard1:
 *      Primary: UNKNWON
 *      Secondary: UNKNOWN
 *  s0: SHARDED - SV1
 *  s1: UNSHARDED (i.e. not present in config.collections)
 */

// If this were to equal 0 it would mean that s1 sent an UNSHARDED version to a stale secondary on
// the new dbPrimary shard1. Being 1 means we're doing the correct thing here.
assert.eq(s1.getDB(dbName).MyColl.find({}).readPref('secondary').itcount(), 1);
assert.eq(s0.getDB(dbName).MyColl.find({}).itcount(), 1);

st.stop();
