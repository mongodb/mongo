/*
 * Tests the changePrimary command to ensure that it does not move any collections but does move
 * views.
 *
 * @tags: [
 *   featureFlagBalanceUnshardedCollections,
 *   # Needed to run createUnsplittableCollection
 *   featureFlagAuthoritativeShardCollection,
 * ]
 */

var st = new ShardingTest({mongos: 1, shards: 2});

var mongos = st.s0;
var shard0 = st.shard0;
var shard1 = st.shard1;
var config = st.config;

const dbName = 'test_db';
const shardedCollName = 'sharded_coll';
const unshardedCollName = 'unsharded_coll';
const untrackedCollName = 'untracked_coll';
const shardedViewName = 'view_sharded';
const unshardedViewName = 'view_unsharded';

function checkAccessViaMongoS() {
    assert.eq(mongos.getDB(dbName).getCollection(shardedCollName).find().itcount(), 1);
    assert.eq(mongos.getDB(dbName).getCollection(unshardedCollName).find().itcount(), 1);
    assert.eq(mongos.getDB(dbName).getCollection(shardedViewName).find().itcount(), 1);
    assert.eq(mongos.getDB(dbName).getCollection(unshardedViewName).find().itcount(), 1);
    assert.eq(mongos.getDB(dbName).getCollection(untrackedCollName).find().itcount(), 1);
}

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));

// Create sharded collection and insert 1 document.
assert.commandWorked(
    mongos.adminCommand({shardCollection: dbName + '.' + shardedCollName, key: {_id: 1}}));
assert.commandWorked(mongos.getDB(dbName).getCollection(shardedCollName).insert({_id: 0}));
// Create unsharded (tracked) collection and insert 1 document.
assert.commandWorked(
    mongos.getDB(dbName).runCommand({createUnsplittableCollection: unshardedCollName}));
assert.commandWorked(mongos.getDB(dbName).getCollection(unshardedCollName).insert({_id: 0}));
// Create unsharded (untracked) collection and insert 1 document.
assert.commandWorked(shard0.getDB(dbName).getCollection(untrackedCollName).insert({_id: 0}));
// Create view on sharded collection.
assert.commandWorked(mongos.getDB(dbName).createView(shardedViewName, shardedCollName, []));
// Create view on unsharded collection.
assert.commandWorked(mongos.getDB(dbName).createView(unshardedViewName, unshardedCollName, []));

jsTest.log('Checking that all commands are routed correctly before changing primary');
checkAccessViaMongoS();

jsTest.log('Checking that collections are on shard 0');
assert.eq(shard0.getDB(dbName).getCollection(shardedCollName).find().itcount(), 1);
assert.eq(shard0.getDB(dbName).getCollection(unshardedCollName).find().itcount(), 1);
assert.eq(shard0.getDB(dbName).getCollection(untrackedCollName).find().itcount(), 1);

jsTest.log('Running change primary');
assert.commandWorked(st.s.adminCommand({changePrimary: dbName, to: shard1.shardName}));

jsTest.log('Checking that all commands are routed correctly after change primary');
checkAccessViaMongoS();

jsTest.log('Checking that collections are still on shard 0');
assert.eq(shard0.getDB(dbName).getCollection(shardedCollName).find().itcount(), 1);
assert.eq(shard0.getDB(dbName).getCollection(unshardedCollName).find().itcount(), 1);

jsTest.log('Checking that untracked collection is now on shard 1');
assert.eq(shard1.getDB(dbName).getCollection(untrackedCollName).find().itcount(), 1);

st.stop();
