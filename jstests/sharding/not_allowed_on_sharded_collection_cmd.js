// Tests that commands that should not be runnable on sharded collections cannot be run on sharded
// collections.
(function() {

    const st = new ShardingTest({shards: 2, mongos: 2});

    const dbName = 'test';
    const coll1 = 'foo';
    const coll2 = 'baz';
    const ns1 = dbName + '.' + coll1;
    const ns2 = dbName + '.' + coll2;

    const freshMongos = st.s0.getDB(dbName);
    const staleMongos = st.s1.getDB(dbName);

    assert.commandWorked(staleMongos.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    staleMongos[coll1].insert({a: 1});
    staleMongos[coll1].insert({a: 2});
    staleMongos[coll1].insert({a: 3});
    staleMongos[coll1].insert({a: 5});
    staleMongos[coll1].insert({a: 7});
    staleMongos[coll2].insert({a: 11});
    staleMongos[coll2].insert({a: 13});
    staleMongos[coll2].insert({a: 17});

    // Test that commands that should not work on sharded collections work on unsharded
    // collections.
    assert.commandWorked(staleMongos.runCommand(
        {group: {ns: coll1, key: {_id: 1}, $reduce: function(curr, result) {}, initial: {}}}));
    assert.commandWorked(staleMongos.runCommand({convertToCapped: coll2, size: 64 * 1024}));

    // Attempt to shard the collections. coll1 should be able to be sharded, whereas coll2 should
    // not, because it has been converted to a capped collection.
    assert.commandWorked(freshMongos.adminCommand({shardCollection: ns1, key: {_id: 1}}));
    assert.commandFailed(freshMongos.adminCommand({shardCollection: ns2, key: {_id: 1}}));

    // Test that commands that should not be runnable on sharded collection do not work on sharded
    // collections, using both fresh mongos and stale mongos instances.
    assert.commandFailedWithCode(
        freshMongos.runCommand(
            {group: {ns: coll1, key: {_id: 1}, $reduce: function(curr, result) {}, initial: {}}}),
        ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(
        staleMongos.runCommand(
            {group: {ns: coll1, key: {_id: 1}, $reduce: function(curr, result) {}, initial: {}}}),
        ErrorCodes.IllegalOperation);

    assert.commandFailedWithCode(freshMongos.runCommand({convertToCapped: coll1, size: 64 * 1024}),
                                 ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(staleMongos.runCommand({convertToCapped: coll1, size: 32 * 1024}),
                                 ErrorCodes.IllegalOperation);

    st.stop();

})();
