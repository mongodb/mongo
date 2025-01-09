/**
 * Test that unsharded collection blocks the removing of the shard and it's correctly
 * shown in the remaining.collectionsToMove counter.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({shards: 2});

var config = st.s.getDB('config');

assert.commandWorked(
    st.s.adminCommand({enableSharding: 'needToMove', primaryShard: st.shard0.shardName}));

var db = st.s.getDB("needToMove");
assert.commandWorked(db.createCollection("coll1"));

// Move unsharded collection to non-primary shard
assert.commandWorked(
    st.s.adminCommand({moveCollection: "needToMove.coll1", toShard: st.shard1.shardName}));

// Initiate removeShard
assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));

// Check the ongoing status and unsharded collection, that cannot be moved
var removeResult = assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
assert.eq(
    'ongoing', removeResult.state, 'Shard should stay in ongoing state: ' + tojson(removeResult));
assert.eq(1, removeResult.remaining.collectionsToMove);
assert.eq(0, removeResult.remaining.dbs);
assert.eq(1, removeResult.collectionsToMove.length);
assert.eq(0, removeResult.dbsToMove.length);

// Check the status once again
removeResult = assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
assert.eq(
    'ongoing', removeResult.state, 'Shard should stay in ongoing state: ' + tojson(removeResult));
assert.eq(1, removeResult.remaining.collectionsToMove);
assert.eq(0, removeResult.remaining.dbs);
assert.eq(1, removeResult.collectionsToMove.length);
assert.eq(0, removeResult.dbsToMove.length);

// Move the collection back to primary shard and check that the other shard can be remove
assert.commandWorked(
    st.s.adminCommand({moveCollection: "needToMove.coll1", toShard: st.shard0.shardName}));
removeResult = assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
assert.eq('completed', removeResult.state, 'Shard was not removed: ' + tojson(removeResult));

st.s0.getDB('needToMove').dropDatabase();

var existingShards = config.shards.find({}).toArray();
assert.eq(
    1, existingShards.length, "Removed server still appears in count: " + tojson(existingShards));

assert.commandFailed(st.s.adminCommand({removeShard: st.shard1.shardName}));

st.stop();
