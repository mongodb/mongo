// Tests the behavior of a $lookup when a shard contains incorrect routing information for the
// local and/or foreign collections.  This includes when the shard thinks the collection is sharded
// when it's not, and likewise when it thinks the collection is unsharded but is actually sharded.
//
// We restart a merizod to cause it to forget that a collection was sharded. When restarted, we
// expect it to still have all the previous data.
// @tags: [requires_persistence]
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

        // Reset the server parameter allowing sharded $lookup on each node.
        setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(rs.getPrimary()),
                               "internalQueryAllowShardedLookup",
                               true);
    }

    const testName = "lookup_stale_merizod";
    const st = new ShardingTest({
        shards: 2,
        merizos: 2,
        rs: {nodes: 1},
    });

    // Set the parameter allowing sharded $lookup on all nodes.
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(st.s0).concat([st.s1.host]),
                           "internalQueryAllowShardedLookup",
                           true);

    const merizos0DB = st.s0.getDB(testName);
    const merizos0LocalColl = merizos0DB[testName + "_local"];
    const merizos0ForeignColl = merizos0DB[testName + "_foreign"];

    const merizos1DB = st.s1.getDB(testName);
    const merizos1LocalColl = merizos1DB[testName + "_local"];
    const merizos1ForeignColl = merizos1DB[testName + "_foreign"];

    const pipeline = [
        {
          $lookup:
              {localField: "a", foreignField: "b", from: merizos0ForeignColl.getName(), as: "same"}
        },
        // Unwind the results of the $lookup, so we can sort by them to get a consistent ordering
        // for the query results.
        {$unwind: "$same"},
        {$sort: {_id: 1, "same._id": 1}}
    ];

    // The results are expected to be correct if the $lookup stage is executed on the merizos which
    // is aware that the collection is sharded.
    const expectedResults = [
        {_id: 0, a: 1, "same": {_id: 0, b: 1}},
        {_id: 1, a: null, "same": {_id: 1, b: null}},
        {_id: 1, a: null, "same": {_id: 2}},
        {_id: 2, "same": {_id: 1, b: null}},
        {_id: 2, "same": {_id: 2}}
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
    // Test unsharded local and sharded foreign collections, with the primary shard unaware that
    // the foreign collection is sharded.
    //

    // Shard the foreign collection.
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

    // Verify $lookup results through the fresh merizos.
    restartPrimaryShard(st.rs0, merizos0LocalColl, merizos0ForeignColl);
    assert.eq(merizos0LocalColl.aggregate(pipeline).toArray(), expectedResults);

    // Verify $lookup results through merizos1, which is not aware that the foreign collection is
    // sharded. In this case the results will be correct since the entire pipeline will be run on a
    // shard, which will do a refresh before executing the foreign pipeline.
    restartPrimaryShard(st.rs0, merizos0LocalColl, merizos0ForeignColl);
    assert.eq(merizos1LocalColl.aggregate(pipeline).toArray(), expectedResults);

    //
    // Test sharded local and sharded foreign collections, with the primary shard unaware that
    // either collection is sharded.
    //

    // Shard the local collection.
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

    // Verify $lookup results through the fresh merizos.
    restartPrimaryShard(st.rs0, merizos0LocalColl, merizos0ForeignColl);
    assert.eq(merizos0LocalColl.aggregate(pipeline).toArray(), expectedResults);

    // Verify $lookup results through merizos1, which is not aware that the local
    // collection is sharded. The results are expected to be incorrect when both the merizos and
    // primary shard incorrectly believe that a collection is unsharded.
    // TODO: This should be fixed by SERVER-32629, likewise for the other aggregates in this file
    // sent to the stale merizos.
    restartPrimaryShard(st.rs0, merizos0LocalColl, merizos0ForeignColl);
    assert.eq(merizos1LocalColl.aggregate(pipeline).toArray(), [
        {_id: 1, a: null, "same": {_id: 1, b: null}},
        {_id: 1, a: null, "same": {_id: 2}},

        {_id: 2, "same": {_id: 1, b: null}},
        {_id: 2, "same": {_id: 2}}
    ]);

    //
    // Test sharded local and unsharded foreign collections, with the primary shard unaware that
    // the local collection is sharded.
    //

    // Recreate the foreign collection as unsharded.
    merizos0ForeignColl.drop();
    assert.writeOK(merizos0ForeignColl.insert({_id: 0, b: 1}));
    assert.writeOK(merizos0ForeignColl.insert({_id: 1, b: null}));
    assert.writeOK(merizos0ForeignColl.insert({_id: 2}));

    // Verify $lookup results through the fresh merizos.
    restartPrimaryShard(st.rs0, merizos0LocalColl, merizos0ForeignColl);
    assert.eq(merizos0LocalColl.aggregate(pipeline).toArray(), expectedResults);

    // Verify $lookup results through merizos1, which is not aware that the local
    // collection is sharded. The results are expected to be incorrect when both the merizos and
    // primary shard incorrectly believe that a collection is unsharded.
    restartPrimaryShard(st.rs0, merizos0LocalColl, merizos0ForeignColl);
    assert.eq(merizos1LocalColl.aggregate(pipeline).toArray(), [
        {_id: 1, a: null, "same": {_id: 1, b: null}},
        {_id: 1, a: null, "same": {_id: 2}},
        {_id: 2, "same": {_id: 1, b: null}},
        {_id: 2, "same": {_id: 2}}
    ]);

    st.stop();
})();
