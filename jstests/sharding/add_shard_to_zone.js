/**
 * Basic integration tests for addShardToZone command. More detailed tests can be found
 * in sharding_catalog_add_shard_to_zone_test.cpp.
 */
(function() {
    var st = new ShardingTest({shards: 1});

    var configDB = st.s.getDB('config');
    var shardName = configDB.shards.findOne()._id;

    // Test adding shard with no zone to a new zone.
    assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: 'x'}));
    var shardDoc = configDB.shards.findOne();
    assert.eq(['x'], shardDoc.tags);

    // Test adding zone to a shard with existing zones.
    assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: 'y'}));
    shardDoc = configDB.shards.findOne();
    assert.eq(['x', 'y'], shardDoc.tags);

    st.stop();
})();
