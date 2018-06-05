//
// Basic tests for movePrimary.
//

(function() {
    'use strict';

    var st = new ShardingTest({mongos: 1, shards: 2});
    var mongos = st.s0;

    var kDbName = 'db';

    var shard0 = st.shard0.shardName;
    var shard1 = st.shard1.shardName;

    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, shard0);
    assert.eq(shard0, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);

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

    // Fail if 'to' shard does not exist or empty.
    assert.commandFailed(mongos.adminCommand({movePrimary: kDbName, to: 'Unknown'}));
    assert.commandFailed(mongos.adminCommand({movePrimary: kDbName, to: ''}));
    assert.commandFailed(mongos.adminCommand({movePrimary: kDbName}));

    let versionBeforeMovePrimary = mongos.getDB('config').databases.findOne({_id: kDbName}).version;

    // Succeed if 'to' shard exists and verify metadata changes.
    assert.eq(shard0, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);
    assert.commandWorked(mongos.adminCommand({movePrimary: kDbName, to: shard1}));
    assert.eq(shard1, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);

    assert.eq(versionBeforeMovePrimary.lastMod + 1,
              mongos.getDB('config').databases.findOne({_id: kDbName}).version.lastMod);
    assert.eq(versionBeforeMovePrimary.uuid,
              mongos.getDB('config').databases.findOne({_id: kDbName}).version.uuid);

    // Succeed if 'to' shard is already the primary shard for the db.
    assert.commandWorked(mongos.adminCommand({movePrimary: kDbName, to: shard1}));
    assert.eq(shard1, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);

    // Verify the version doesn't change if the 'to' shard is already the primary shard.
    assert.eq(versionBeforeMovePrimary.lastMod + 1,
              mongos.getDB('config').databases.findOne({_id: kDbName}).version.lastMod);
    assert.eq(versionBeforeMovePrimary.uuid,
              mongos.getDB('config').databases.findOne({_id: kDbName}).version.uuid);

    st.stop();
})();
