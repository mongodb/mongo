// Test that shardCollection uses existing zone info to validate
// shard keys.
(function() {
    'use strict';

    var st = new ShardingTest({mongos: 1, shards: 1});
    var kDbName = 'test';
    var ns = kDbName + '.foo';
    var zoneName = 'zoneName';
    var mongos = st.s0;
    var testDB = mongos.getDB(kDbName);
    var configDB = mongos.getDB('config');
    var shardName = st.shard0.shardName;
    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

    /**
     * Test that shardCollection correctly validates shard key against existing zones.
     */
    function testShardCollection(proposedShardKey, numberLongMin, numberLongMax, success) {
        assert.commandWorked(testDB.foo.createIndex(proposedShardKey));
        assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: zoneName}));

        var zoneMin = numberLongMin ? {x: NumberLong(0)} : {x: 0};
        var zoneMax = numberLongMax ? {x: NumberLong(10)} : {x: 10};
        assert.commandWorked(st.s.adminCommand(
            {updateZoneKeyRange: ns, min: zoneMin, max: zoneMax, zone: zoneName}));

        var tagDoc = configDB.tags.findOne();
        assert.eq(ns, tagDoc.ns);
        assert.eq(zoneMin, tagDoc.min);
        assert.eq(zoneMax, tagDoc.max);
        assert.eq(zoneName, tagDoc.tag);

        if (success) {
            assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: proposedShardKey}));
        } else {
            assert.commandFailed(mongos.adminCommand({shardCollection: ns, key: proposedShardKey}));
        }

        testDB.foo.drop();
    }

    testShardCollection({x: 1}, false, false, true);

    // cannot use a completely different key from the zone shard key or a key
    // that has the zone shard key as a prefix is not allowed.
    testShardCollection({y: 1}, false, false, false);
    testShardCollection({x: 1, y: 1}, false, false, false);

    // can only do hash sharding when the boundaries are of type NumberLong.
    testShardCollection({x: "hashed"}, false, false, false);
    testShardCollection({x: "hashed"}, true, false, false);
    testShardCollection({x: "hashed"}, false, true, false);
    testShardCollection({x: "hashed"}, true, true, true);

    st.stop();

})();
