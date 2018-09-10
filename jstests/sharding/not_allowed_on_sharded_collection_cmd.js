// Tests that commands that should not be runnable on sharded collections cannot be run on sharded
// collections.
(function() {

    const st = new ShardingTest({shards: 2, mongos: 2});

    const dbName = 'test';
    const coll = 'foo';
    const ns = dbName + '.' + coll;

    const freshMongos = st.s0.getDB(dbName);
    const staleMongos = st.s1.getDB(dbName);

    assert.commandWorked(staleMongos.adminCommand({enableSharding: dbName}));
    assert.commandWorked(freshMongos.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Test that commands that should not be runnable on sharded collection do not work on sharded
    // collections, using both fresh mongos and stale mongos instances.
    assert.commandFailedWithCode(freshMongos.runCommand({convertToCapped: coll, size: 64 * 1024}),
                                 ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(staleMongos.runCommand({convertToCapped: coll, size: 32 * 1024}),
                                 ErrorCodes.IllegalOperation);

    st.stop();

})();
