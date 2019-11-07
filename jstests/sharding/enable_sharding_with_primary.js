//
// Enable sharding with custom primary shard tests
//

(function() {
    'use strict';

    var st = new ShardingTest({mongos: 1, shards: 2});

    // Can't enable sharding on a database using a wrong shard name
    assert.commandFailed(st.s0.adminCommand(
        {enableSharding: 'db2', primaryShard: st.shard1.shardName + '_unenxisting_name_postfix'}));

    // Enabling sharding on a database with a valid shard name must work
    assert.commandWorked(
        st.s0.adminCommand({enableSharding: 'db2', primaryShard: st.shard1.shardName}));
    assert.eq(st.s0.getDB('config').databases.findOne({_id: 'db2'}).primary, st.shard1.shardName);

    // Enable sharding on a database already created with the correct primary shard name must work
    assert.commandWorked(
        st.s0.adminCommand({enableSharding: 'db2', primaryShard: st.shard1.shardName}));

    // Can't enable sharding of a database already created with a different primary shard name
    assert.commandFailedWithCode(
        st.s0.adminCommand({enableSharding: 'db2', primaryShard: st.shard0.shardName}),
        ErrorCodes.NamespaceExists);

    st.stop();
})();