// Tests the behavior of a $lookup when the merizos contains stale routing information for the
// local and/or foreign collections.  This includes when merizos thinks the collection is sharded
// when it's not, and likewise when merizos thinks the collection is unsharded but is actually
// sharded.
(function() {
    "use strict";

    load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
    load("jstests/libs/discover_topology.js");                       // For findDataBearingNodes.

    const testName = "lookup_stale_merizos";
    const st = new ShardingTest({
        shards: 2,
        merizos: 2,
    });
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(st.s0).concat([st.s1.host]),
                           "internalQueryAllowShardedLookup",
                           true);

    const merizos0DB = st.s0.getDB(testName);
    assert.commandWorked(merizos0DB.dropDatabase());
    const merizos0LocalColl = merizos0DB[testName + "_local"];
    const merizos0ForeignColl = merizos0DB[testName + "_foreign"];

    const merizos1DB = st.s1.getDB(testName);
    const merizos1LocalColl = merizos1DB[testName + "_local"];
    const merizos1ForeignColl = merizos1DB[testName + "_foreign"];

    const pipeline = [
        {
          $lookup:
              {localField: "a", foreignField: "b", from: merizos1ForeignColl.getName(), as: "same"}
        },
        {$sort: {_id: 1}}
    ];
    const expectedResults = [
        {_id: 0, a: 1, "same": [{_id: 0, b: 1}]},
        {_id: 1, a: null, "same": [{_id: 1, b: null}, {_id: 2}]},
        {_id: 2, "same": [{_id: 1, b: null}, {_id: 2}]}
    ];

    // Ensure that shard0 is the primary shard.
    assert.commandWorked(merizos0DB.adminCommand({enableSharding: merizos0DB.getName()}));
    st.ensurePrimaryShard(merizos0DB.getName(), st.shard0.shardName);

    assert.writeOK(merizos0LocalColl.insert({_id: 0, a: 1}));
    assert.writeOK(merizos0LocalColl.insert({_id: 1, a: null}));

    assert.writeOK(merizos0ForeignColl.insert({_id: 0, b: 1}));
    assert.writeOK(merizos0ForeignColl.insert({_id: 1, b: null}));

    // Send writes through merizos1 such that it's aware of the collections and believes they are
    // unsharded.
    assert.writeOK(merizos1LocalColl.insert({_id: 2}));
    assert.writeOK(merizos1ForeignColl.insert({_id: 2}));

    //
    // Test unsharded local and sharded foreign collections, with merizos unaware that the foreign
    // collection is sharded.
    //

    // Shard the foreign collection through merizos0.
    assert.commandWorked(
        merizos0DB.adminCommand({shardCollection: merizos0ForeignColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 2 chunks: [MinKey, 1), [1, MaxKey).
    assert.commandWorked(
        merizos0DB.adminCommand({split: merizos0ForeignColl.getFullName(), middle: {_id: 1}}));

    // Move the [minKey, 1) chunk to shard1.
    assert.commandWorked(merizos0DB.adminCommand({
        moveChunk: merizos0ForeignColl.getFullName(),
        find: {_id: 0},
        to: st.shard1.shardName,
        _waitForDelete: true
    }));

    // Issue a $lookup through merizos1, which is unaware that the foreign collection is sharded.
    assert.eq(merizos1LocalColl.aggregate(pipeline).toArray(), expectedResults);

    //
    // Test sharded local and sharded foreign collections, with merizos unaware that the local
    // collection is sharded.
    //

    // Shard the local collection through merizos0.
    assert.commandWorked(
        merizos0DB.adminCommand({shardCollection: merizos0LocalColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 2 chunks: [MinKey, 1), [1, MaxKey).
    assert.commandWorked(
        merizos0DB.adminCommand({split: merizos0LocalColl.getFullName(), middle: {_id: 1}}));

    // Move the [minKey, 1) chunk to shard1.
    assert.commandWorked(merizos0DB.adminCommand({
        moveChunk: merizos0LocalColl.getFullName(),
        find: {_id: 0},
        to: st.shard1.shardName,
        _waitForDelete: true
    }));

    // Issue a $lookup through merizos1, which is unaware that the local collection is sharded.
    assert.eq(merizos1LocalColl.aggregate(pipeline).toArray(), expectedResults);

    //
    // Test sharded local and unsharded foreign collections, with merizos unaware that the foreign
    // collection is unsharded.
    //

    // Recreate the foreign collection as unsharded through merizos0.
    merizos0ForeignColl.drop();
    assert.writeOK(merizos0ForeignColl.insert({_id: 0, b: 1}));
    assert.writeOK(merizos0ForeignColl.insert({_id: 1, b: null}));
    assert.writeOK(merizos0ForeignColl.insert({_id: 2}));

    // Issue a $lookup through merizos1, which is unaware that the foreign collection is now
    // unsharded.
    assert.eq(merizos1LocalColl.aggregate(pipeline).toArray(), expectedResults);

    //
    // Test unsharded local and foreign collections, with merizos unaware that the local
    // collection is unsharded.
    //

    // Recreate the local collection as unsharded through merizos0.
    merizos0LocalColl.drop();
    assert.writeOK(merizos0LocalColl.insert({_id: 0, a: 1}));
    assert.writeOK(merizos0LocalColl.insert({_id: 1, a: null}));
    assert.writeOK(merizos0LocalColl.insert({_id: 2}));

    // Issue a $lookup through merizos1, which is unaware that the local collection is now
    // unsharded.
    assert.eq(merizos1LocalColl.aggregate(pipeline).toArray(), expectedResults);

    st.stop();
})();
