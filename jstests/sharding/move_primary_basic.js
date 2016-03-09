//
// Basic tests for movePrimary.
//

(function() {
    'use strict';

    var st = new ShardingTest({mongos: 1, shards: 2});

    var mongos = st.s0;

    var kDbName = 'db';

    var shards = mongos.getCollection('config.shards').find().toArray();

    var shard0 = shards[0]._id;
    var shard1 = shards[1]._id;

    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, shard0);
    assert.eq(shard0, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);

    // Can run only on mongos.
    assert.commandFailedWithCode(
        st.d0.getDB('admin').runCommand({movePrimary: kDbName, to: shard0}),
        ErrorCodes.CommandNotFound);

    // Can run only against the admin database.
    assert.commandFailedWithCode(
        mongos.getDB('test').runCommand({movePrimary: kDbName, to: shard0}),
        ErrorCodes.Unauthorized);

    // Can't movePrimary for 'config' database.
    assert.commandFailed(mongos.adminCommand({movePrimary: 'config', to: shard0}));

    // Can't movePrimary for 'local' database.
    assert.commandFailed(mongos.adminCommand({movePrimary: 'local', to: shard0}));

    // Can't movePrimary for 'admin' database.
    assert.commandFailed(mongos.adminCommand({movePrimary: 'admin', to: shard0}));

    // Can't movePrimary for invalid db name.
    assert.commandFailed(mongos.adminCommand({movePrimary: 'a.b', to: shard0}));
    assert.commandFailed(mongos.adminCommand({movePrimary: '', to: shard0}));

    // Fail if shard does not exist or empty.
    assert.commandFailed(mongos.adminCommand({movePrimary: kDbName, to: 'Unknown'}));
    assert.commandFailed(mongos.adminCommand({movePrimary: kDbName, to: ''}));
    assert.commandFailed(mongos.adminCommand({movePrimary: kDbName}));

    // Fail if moveShard to already primary and verify metadata changes.
    assert.eq(shard0, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);

    assert.commandWorked(mongos.adminCommand({movePrimary: kDbName, to: shard1}));
    assert.eq(shard1, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);

    assert.commandFailed(mongos.adminCommand({movePrimary: kDbName, to: shard1}));
    assert.eq(shard1, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);

    st.stop();

})();
