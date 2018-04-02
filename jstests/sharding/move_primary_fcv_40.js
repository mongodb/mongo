//
// Tests for the FCV 4.0 version of movePrimary.
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

    // If we are in a mixed version suite (sharding_last_stable_mongos_and_mixed_shards) we may not
    // get a version back. If not, we will skip this part of the test.
    let versionBeforeMovePrimary = null;
    if (mongos.getDB('config').databases.findOne({_id: kDbName}).version) {
        versionBeforeMovePrimary = mongos.getDB('config').databases.findOne({_id: kDbName}).version;
    }

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

    // Fail if 'to' shard is empty.
    assert.commandFailed(mongos.adminCommand({movePrimary: kDbName, to: ''}));
    assert.commandFailed(mongos.adminCommand({movePrimary: kDbName}));

    // Succeed if 'to' shard exists and verify metadata changes.
    assert.eq(shard0, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);
    assert.commandWorked(mongos.adminCommand({movePrimary: kDbName, to: shard1}));

    assert.eq(shard1, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);

    if (versionBeforeMovePrimary) {
        assert.eq(versionBeforeMovePrimary.lastMod + 1,
                  mongos.getDB('config').databases.findOne({_id: kDbName}).version.lastMod);
        assert.eq(versionBeforeMovePrimary.uuid,
                  mongos.getDB('config').databases.findOne({_id: kDbName}).version.uuid);
    }

    st.stop();

})();
