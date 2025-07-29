/**
 * Test that unsharded collection blocks the removing of the shard and it's correctly
 * shown in the remaining.collectionsToMove counter.
 */

var st = new ShardingTest({shards: 2});
var config = st.s.getDB('config');
const adminDB = st.s.getDB("admin");

const db0 = st.s.getDB("db0");
const db1 = st.s.getDB("db1");

assert.commandWorked(
    st.s.adminCommand({enableSharding: db0.getName(), primaryShard: st.shard0.shardName}));
assert.commandWorked(
    st.s.adminCommand({enableSharding: db1.getName(), primaryShard: st.shard1.shardName}));

// Create the following collections:
//
//          SHARD0        |   SHARD1 (toRemove)
//  ----------------------+----------------------
//     db0.collUnsharded  |   db1.collUnsharded
//    db0.collTimeseries  |  db1.collTimeseries
//      db0.collSharded   |    db1.collSharded
//   db1.collOutOfPrimary | db0.collOutOfPrimary
//

const expectedCollectionsOnTheDrainingShard =
    ["db1.collUnsharded", "db1.collTimeseries", "db0.collOutOfPrimary"];

[db0, db1].forEach((db) => {
    assert.commandWorked(db.createCollection("collUnsharded"));
    assert.commandWorked(db.createCollection("collOutOfPrimary"));
    assert.commandWorked(db.createCollection("collTimeseries", {timeseries: {timeField: 'time'}}));
    assert.commandWorked(
        db.adminCommand({shardCollection: db.getName() + ".collSharded", key: {x: 1}}));
});

// Move unsharded collections to non-primary shard
assert.commandWorked(
    st.s.adminCommand({moveCollection: "db0.collOutOfPrimary", toShard: st.shard1.shardName}));
assert.commandWorked(
    st.s.adminCommand({moveCollection: "db1.collOutOfPrimary", toShard: st.shard0.shardName}));

// Initiate removeShard
assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));

// Check the ongoing status and unsharded collection, that cannot be moved
var removeResult = assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
assert.eq(
    'ongoing', removeResult.state, 'Shard should stay in ongoing state: ' + tojson(removeResult));
assert.eq(3, removeResult.remaining.collectionsToMove);
assert.eq(1, removeResult.remaining.dbs);
assert.eq(3, removeResult.collectionsToMove.length);
assert.eq(1, removeResult.dbsToMove.length);
assert.sameMembers(expectedCollectionsOnTheDrainingShard, removeResult.collectionsToMove);

// Check the status once again
removeResult = assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
assert.eq(
    'ongoing', removeResult.state, 'Shard should stay in ongoing state: ' + tojson(removeResult));
assert.eq(3, removeResult.remaining.collectionsToMove);
assert.eq(1, removeResult.remaining.dbs);
assert.eq(3, removeResult.collectionsToMove.length);
assert.eq(1, removeResult.dbsToMove.length);
assert.sameMembers(expectedCollectionsOnTheDrainingShard, removeResult.collectionsToMove);

// Move unsharded collections out from the draining shard
expectedCollectionsOnTheDrainingShard.forEach((collName) => {
    adminDB.adminCommand({moveCollection: collName, toShard: st.shard0.shardName});
});

// Move `db1.collSharded` chunk out from the draining shard
adminDB.adminCommand({
    moveRange: "db1.collSharded",
    min: {x: MinKey()},
    max: {x: MaxKey()},
    toShard: st.shard0.shardName
});

// Move `db1` out from the draining shard
adminDB.adminCommand({movePrimary: "db1", to: st.shard0.shardName});

// Finalize removing the shard
removeResult = assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
assert.eq('completed', removeResult.state, 'Shard was not removed: ' + tojson(removeResult));

var existingShards = config.shards.find({}).toArray();
assert.eq(
    1, existingShards.length, "Removed server still appears in count: " + tojson(existingShards));

assert.commandFailed(st.s.adminCommand({removeShard: st.shard1.shardName}));

st.stop();
