/**
 * Basic integration tests for removeShardFromZone command. More detailed tests can be found
 * in sharding_catalog_remove_shard_from_zone_test.cpp.
 */
(function() {
    "use strict";

    var st = new ShardingTest({shards: 1});

    var configDB = st.s.getDB('config');
    var shardName = st.shard0.shardName;

    assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: 'x'}));
    var shardDoc = configDB.shards.findOne();
    assert.eq(['x'], shardDoc.tags);

    // Test removing shard from existing zone.
    assert.commandWorked(st.s.adminCommand({removeShardFromZone: shardName, zone: 'x'}));
    shardDoc = configDB.shards.findOne();
    assert.eq([], shardDoc.tags);

    // Test removing shard from zone that no longer exists.
    assert.commandWorked(st.s.adminCommand({removeShardFromZone: shardName, zone: 'x'}));
    shardDoc = configDB.shards.findOne();
    assert.eq([], shardDoc.tags);

    st.stop();
})();
