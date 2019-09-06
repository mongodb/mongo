/**
 * Tests that shard removal triggers an update of the catalog cache so that routers don't continue
 * to target shards that have been removed.
 */
(function() {
'use strict';

const dbName = 'TestDB';
const shardedCollName = 'Coll';
const shardedCollNs = dbName + '.' + shardedCollName;
const unshardedCollName = 'UnshardedColl';
const unshardedCollNs = dbName + '.' + unshardedCollName;

var st = new ShardingTest({shards: 2, mongos: 2});
let router0DB = st.s0.getDB(dbName);
let router1DB = st.s1.getDB(dbName);

let router0ShardedColl = router0DB[shardedCollName];
let router1ShardedColl = router1DB[shardedCollName];

let router0UnshardedColl = router0DB[unshardedCollName];
let router1UnshardedColl = router1DB[unshardedCollName];

assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s0.adminCommand({shardCollection: shardedCollNs, key: {_id: 1}}));
assert.commandWorked(st.s0.adminCommand({split: shardedCollNs, middle: {_id: 0}}));

// Insert some documents into the sharded collection and make sure there are documents on both
// shards.
router0ShardedColl.insert({_id: -1, value: 'Negative value'});
router0ShardedColl.insert({_id: 1, value: 'Positive value'});
assert.commandWorked(st.s0.adminCommand(
    {moveChunk: shardedCollNs, find: {_id: -1}, to: st.shard0.shardName, _waitForDelete: true}));
assert.commandWorked(st.s0.adminCommand(
    {moveChunk: shardedCollNs, find: {_id: 1}, to: st.shard1.shardName, _waitForDelete: true}));

// Insert some documents into the unsharded collection whose primary is the to-be-removed shard0.
router0UnshardedColl.insert({_id: 1, value: 'Positive value'});

// Force s0 and s1 to load the database and collection cache entries for both the sharded and
// unsharded collections.
assert.eq(2, router0ShardedColl.find({}).itcount());
assert.eq(2, router1ShardedColl.find({}).itcount());
assert.eq(1, router0UnshardedColl.find({}).itcount());
assert.eq(1, router1UnshardedColl.find({}).itcount());

// Add new shard.
var newshard = MongoRunner.runMongod({'shardsvr': ''});
assert.commandWorked(st.s0.adminCommand({addShard: newshard.host, name: 'zzz-shard'}));
assert.commandWorked(st.s0.adminCommand({movePrimary: dbName, to: 'zzz-shard'}));

// Start the balancer here so that it can drain shards when they're removed but also won't conflict
// with the above moveChunk commands.
st.startBalancer();

// Remove shard1.
assert.soon(() => {
    const removeRes = assert.commandWorked(st.s0.adminCommand({removeShard: st.shard1.shardName}));
    return 'completed' === removeRes.state;
});

// Remove shard0.
assert.soon(() => {
    const removeRes = assert.commandWorked(st.s0.adminCommand({removeShard: st.shard0.shardName}));
    return 'completed' === removeRes.state;
});

// Ensure that s1, the router which did not run removeShard, eventually stops targeting chunks for
// the sharded collection which previously resided on shards that no longer exists.
assert.soon(() => {
    try {
        const response = router1ShardedColl.explain().count({_id: 1});
        return response.ok;
    } catch (e) {
        print(e);
        return false;
    }
});

// Ensure that s1, the router which did not run removeShard, eventually stops targeting data for
// the unsharded collection which previously had as primary a shard that no longer exist.
assert.soon(() => {
    try {
        const response = router1UnshardedColl.explain().count({_id: 1});
        return response.ok;
    } catch (e) {
        print(e);
        return false;
    }
});

st.stop();
MongoRunner.stopMongod(newshard);
})();
