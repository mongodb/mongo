// Tests the behavior of a $lookup when the mongos contains stale routing information for the
// local and/or foreign collections.  This includes when mongos thinks the collection is sharded
// when it's not, and likewise when mongos thinks the collection is unsharded but is actually
// sharded.
(function() {
    "use strict";

    load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
    load("jstests/libs/discover_topology.js");                       // For findDataBearingNodes.

    const testName = "lookup_stale_mongos";
    const st = new ShardingTest({
        shards: 2,
        mongos: 2,
    });
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(st.s0).concat([st.s1.host]),
                           "internalQueryAllowShardedLookup",
                           true);

    const mongos0DB = st.s0.getDB(testName);
    assert.commandWorked(mongos0DB.dropDatabase());
    const mongos0LocalColl = mongos0DB[testName + "_local"];
    const mongos0ForeignColl = mongos0DB[testName + "_foreign"];

    const mongos1DB = st.s1.getDB(testName);
    const mongos1LocalColl = mongos1DB[testName + "_local"];
    const mongos1ForeignColl = mongos1DB[testName + "_foreign"];

    const pipeline = [
        {
          $lookup:
              {localField: "a", foreignField: "b", from: mongos1ForeignColl.getName(), as: "same"}
        },
        {$sort: {_id: 1}}
    ];
    const expectedResults = [
        {_id: 0, a: 1, "same": [{_id: 0, b: 1}]},
        {_id: 1, a: null, "same": [{_id: 1, b: null}, {_id: 2}]},
        {_id: 2, "same": [{_id: 1, b: null}, {_id: 2}]}
    ];

    // Ensure that shard0 is the primary shard.
    assert.commandWorked(mongos0DB.adminCommand({enableSharding: mongos0DB.getName()}));
    st.ensurePrimaryShard(mongos0DB.getName(), st.shard0.shardName);

    assert.writeOK(mongos0LocalColl.insert({_id: 0, a: 1}));
    assert.writeOK(mongos0LocalColl.insert({_id: 1, a: null}));

    assert.writeOK(mongos0ForeignColl.insert({_id: 0, b: 1}));
    assert.writeOK(mongos0ForeignColl.insert({_id: 1, b: null}));

    // Send writes through mongos1 such that it's aware of the collections and believes they are
    // unsharded.
    assert.writeOK(mongos1LocalColl.insert({_id: 2}));
    assert.writeOK(mongos1ForeignColl.insert({_id: 2}));

    //
    // Test unsharded local and sharded foreign collections, with mongos unaware that the foreign
    // collection is sharded.
    //

    // Shard the foreign collection through mongos0.
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

    // Issue a $lookup through mongos1, which is unaware that the foreign collection is sharded.
    assert.eq(mongos1LocalColl.aggregate(pipeline).toArray(), expectedResults);

    //
    // Test sharded local and sharded foreign collections, with mongos unaware that the local
    // collection is sharded.
    //

    // Shard the local collection through mongos0.
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

    // Issue a $lookup through mongos1, which is unaware that the local collection is sharded.
    assert.eq(mongos1LocalColl.aggregate(pipeline).toArray(), expectedResults);

    //
    // Test sharded local and unsharded foreign collections, with mongos unaware that the foreign
    // collection is unsharded.
    //

    // Recreate the foreign collection as unsharded through mongos0.
    mongos0ForeignColl.drop();
    assert.writeOK(mongos0ForeignColl.insert({_id: 0, b: 1}));
    assert.writeOK(mongos0ForeignColl.insert({_id: 1, b: null}));
    assert.writeOK(mongos0ForeignColl.insert({_id: 2}));

    // Issue a $lookup through mongos1, which is unaware that the foreign collection is now
    // unsharded.
    assert.eq(mongos1LocalColl.aggregate(pipeline).toArray(), expectedResults);

    //
    // Test unsharded local and foreign collections, with mongos unaware that the local
    // collection is unsharded.
    //

    // Recreate the local collection as unsharded through mongos0.
    mongos0LocalColl.drop();
    assert.writeOK(mongos0LocalColl.insert({_id: 0, a: 1}));
    assert.writeOK(mongos0LocalColl.insert({_id: 1, a: null}));
    assert.writeOK(mongos0LocalColl.insert({_id: 2}));

    // Issue a $lookup through mongos1, which is unaware that the local collection is now
    // unsharded.
    assert.eq(mongos1LocalColl.aggregate(pipeline).toArray(), expectedResults);

    st.stop();
})();
