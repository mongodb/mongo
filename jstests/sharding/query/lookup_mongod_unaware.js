/**
 * Tests the behavior of a $lookup when a shard contains incorrect routing information for the
 * local and/or foreign collections. This includes when the shard thinks the collection is sharded
 * when it's not, and likewise when it thinks the collection is unsharded but is actually sharded.
 *
 * We restart a mongod to cause it to forget that a collection was sharded. When restarted, we
 * expect it to still have all the previous data.
 *
 * @tags: [
 *  requires_persistence
 * ]
 *
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
load("jstests/libs/discover_topology.js");                       // For findDataBearingNodes.

// Restarts the primary shard and ensures that it believes both collections are unsharded.
function restartPrimaryShard(rs, localColl, foreignColl) {
    // Returns true if the shard is aware that the collection is sharded.
    function hasRoutingInfoForNs(shardConn, coll) {
        const res = shardConn.adminCommand({getShardVersion: coll, fullMetadata: true});
        assert.commandWorked(res);
        return res.metadata.collVersion != undefined;
    }

    rs.restart(0);
    rs.awaitSecondaryNodes();
    assert(!hasRoutingInfoForNs(rs.getPrimary(), localColl.getFullName()));
    assert(!hasRoutingInfoForNs(rs.getPrimary(), foreignColl.getFullName()));
}

// Disable checking for index consistency to ensure that the config server doesn't trigger a
// StaleShardVersion exception on shard0 and cause it to refresh its sharding metadata.
const nodeOptions = {
    setParameter: {enableShardedIndexConsistencyCheck: false}
};

const testName = "lookup_stale_mongod";
const st =
    new ShardingTest({shards: 2, mongos: 2, rs: {nodes: 1}, other: {configOptions: nodeOptions}});

const mongos0DB = st.s0.getDB(testName);
const mongos0LocalColl = mongos0DB[testName + "_local"];
const mongos0ForeignColl = mongos0DB[testName + "_foreign"];

const mongos1DB = st.s1.getDB(testName);
const mongos1LocalColl = mongos1DB[testName + "_local"];
const mongos1ForeignColl = mongos1DB[testName + "_foreign"];

const pipeline = [
    {$lookup: {localField: "a", foreignField: "b", from: mongos0ForeignColl.getName(), as: "same"}},
    // Unwind the results of the $lookup, so we can sort by them to get a consistent ordering
    // for the query results.
    {$unwind: "$same"},
    {$sort: {_id: 1, "same._id": 1}}
];

// The results are expected to be correct if the $lookup stage is executed on the mongos which
// is aware that the collection is sharded.
const expectedResults = [
    {_id: 0, a: 1, "same": {_id: 0, b: 1}},
    {_id: 1, a: null, "same": {_id: 1, b: null}},
    {_id: 1, a: null, "same": {_id: 2}},
    {_id: 2, "same": {_id: 1, b: null}},
    {_id: 2, "same": {_id: 2}}
];

// Ensure that shard0 is the primary shard.
assert.commandWorked(mongos0DB.adminCommand({enableSharding: mongos0DB.getName()}));
st.ensurePrimaryShard(mongos0DB.getName(), st.shard0.shardName);

assert.commandWorked(mongos0LocalColl.insert({_id: 0, a: 1}));
assert.commandWorked(mongos0LocalColl.insert({_id: 1, a: null}));

assert.commandWorked(mongos0ForeignColl.insert({_id: 0, b: 1}));
assert.commandWorked(mongos0ForeignColl.insert({_id: 1, b: null}));

// Send writes through mongos1 such that it's aware of the collections and believes they are
// unsharded.
assert.commandWorked(mongos1LocalColl.insert({_id: 2}));
assert.commandWorked(mongos1ForeignColl.insert({_id: 2}));

//
// Test unsharded local and sharded foreign collections, with the primary shard unaware that
// the foreign collection is sharded.
//

// Shard the foreign collection.
assert.commandWorked(
    mongos0DB.adminCommand({shardCollection: mongos0ForeignColl.getFullName(), key: {_id: 1}}));

// Split the collection into 2 chunks: [MinKey, 1), [1, MaxKey).
assert.commandWorked(
    mongos0DB.adminCommand({split: mongos0ForeignColl.getFullName(), middle: {_id: 1}}));

// Move the [minKey, 1) chunk to shard1.
assert.commandWorked(mongos0DB.adminCommand({
    moveChunk: mongos0ForeignColl.getFullName(),
    find: {_id: 0},
    to: st.shard1.shardName,
    _waitForDelete: true
}));

// Verify $lookup results through the fresh mongos.
restartPrimaryShard(st.rs0, mongos0LocalColl, mongos0ForeignColl);
assert.eq(mongos0LocalColl.aggregate(pipeline).toArray(), expectedResults);

// Verify $lookup results through mongos1, which is not aware that the foreign collection is
// sharded. In this case the results will be correct since the entire pipeline will be run on a
// shard, which will do a refresh before executing the foreign pipeline.
restartPrimaryShard(st.rs0, mongos0LocalColl, mongos0ForeignColl);
assert.eq(mongos1LocalColl.aggregate(pipeline).toArray(), expectedResults);

//
// Test sharded local and sharded foreign collections, with the primary shard unaware that
// either collection is sharded.
//

// Shard the local collection.
assert.commandWorked(
    mongos0DB.adminCommand({shardCollection: mongos0LocalColl.getFullName(), key: {_id: 1}}));

// Split the collection into 2 chunks: [MinKey, 1), [1, MaxKey).
assert.commandWorked(
    mongos0DB.adminCommand({split: mongos0LocalColl.getFullName(), middle: {_id: 1}}));

// Move the [minKey, 1) chunk to shard1.
assert.commandWorked(mongos0DB.adminCommand({
    moveChunk: mongos0LocalColl.getFullName(),
    find: {_id: 0},
    to: st.shard1.shardName,
    _waitForDelete: true
}));

// Verify $lookup results through the fresh mongos.
restartPrimaryShard(st.rs0, mongos0LocalColl, mongos0ForeignColl);
assert.eq(mongos0LocalColl.aggregate(pipeline).toArray(), expectedResults);

// Verify $lookup results through the stale mongos.
restartPrimaryShard(st.rs0, mongos0LocalColl, mongos0ForeignColl);
assert.eq(mongos1LocalColl.aggregate(pipeline).toArray(), expectedResults);

//
// Test sharded local and unsharded foreign collections, with the primary shard unaware that
// the local collection is sharded.
//

// Recreate the foreign collection as unsharded.
mongos0ForeignColl.drop();
assert.commandWorked(mongos0ForeignColl.insert({_id: 0, b: 1}));
assert.commandWorked(mongos0ForeignColl.insert({_id: 1, b: null}));
assert.commandWorked(mongos0ForeignColl.insert({_id: 2}));

// Verify $lookup results through the fresh mongos.
restartPrimaryShard(st.rs0, mongos0LocalColl, mongos0ForeignColl);
assert.eq(mongos0LocalColl.aggregate(pipeline).toArray(), expectedResults);

// Verify $lookup results through the stale mongos.
restartPrimaryShard(st.rs0, mongos0LocalColl, mongos0ForeignColl);
assert.eq(mongos1LocalColl.aggregate(pipeline).toArray(), expectedResults);

st.stop();
})();
