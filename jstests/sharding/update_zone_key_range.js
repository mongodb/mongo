/**
 * Basic integration tests for updateZoneKeyRange command. More detailed tests can be found
 * in sharding_catalog_assign_key_range_to_zone_test.cpp.
 */
(function() {
    var st = new ShardingTest({shards: 1});

    var configDB = st.s.getDB('config');
    var shardName = configDB.shards.findOne()._id;

    assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: 'x'}));
    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.user', key: {x: 1}}));

    // Testing basic assign.
    assert.commandWorked(
        st.s.adminCommand({updateZoneKeyRange: 'test.user', min: {x: 0}, max: {x: 10}, zone: 'x'}));

    var tagDoc = configDB.tags.findOne();

    assert.eq('test.user', tagDoc.ns);
    assert.eq({x: 0}, tagDoc.min);
    assert.eq({x: 10}, tagDoc.max);
    assert.eq('x', tagDoc.tag);

    // Cannot assign overlapping ranges
    assert.commandFailedWithCode(
        st.s.adminCommand(
            {updateZoneKeyRange: 'test.user', min: {x: -10}, max: {x: 20}, zone: 'x'}),
        ErrorCodes.RangeOverlapConflict);

    tagDoc = configDB.tags.findOne();
    assert.eq('test.user', tagDoc.ns);
    assert.eq({x: 0}, tagDoc.min);
    assert.eq({x: 10}, tagDoc.max);
    assert.eq('x', tagDoc.tag);

    // Testing basic remove.
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: 'test.user', min: {x: 0}, max: {x: 10}, zone: null}));

    assert.eq(null, configDB.tags.findOne());

    st.stop();
})();
