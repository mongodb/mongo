/**
 * Basic integration tests for addShardToZone command. More detailed tests can be found
 * in sharding_catalog_add_shard_to_zone_test.cpp.
 */
(function() {
    'use strict';

    let st = new ShardingTest({shards: 1});
    let mongos = st.s0;

    let config = mongos.getDB('config');
    var shardName = st.shard0.shardName;

    // Test adding shard with no zone to a new zone.
    assert.commandWorked(mongos.adminCommand({addShardToZone: shardName, zone: 'x'}));
    var shardDoc = config.shards.findOne();
    assert.eq(['x'], shardDoc.tags);

    // Test adding zone to a shard with existing zones.
    assert.commandWorked(mongos.adminCommand({addShardToZone: shardName, zone: 'y'}));
    shardDoc = config.shards.findOne();
    assert.eq(['x', 'y'], shardDoc.tags);

    // Test removing shard from existing zone.
    assert.commandWorked(mongos.adminCommand({removeShardFromZone: shardName, zone: 'x'}));
    shardDoc = config.shards.findOne();
    assert.eq(['y'], shardDoc.tags);

    // Test removing shard from zone that no longer exists.
    assert.commandWorked(mongos.adminCommand({removeShardFromZone: shardName, zone: 'x'}));
    shardDoc = config.shards.findOne();
    assert.eq(['y'], shardDoc.tags);

    // Test removing the last zone from a shard
    assert.commandWorked(mongos.adminCommand({removeShardFromZone: shardName, zone: 'y'}));
    shardDoc = config.shards.findOne();
    assert.eq([], shardDoc.tags);

    st.stop();
})();
