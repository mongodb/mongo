//
// Basic tests for movePrimary.
//

(function() {
    'use strict';

    function movePrimary(useFCV40) {
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
            mongos.getDB('test').runCommand({movePrimary: kDbName, to: shard0, forTest: useFCV40}),
            ErrorCodes.Unauthorized);

        // Can't movePrimary for 'config' database.
        assert.commandFailed(
            mongos.adminCommand({movePrimary: 'config', to: shard0, forTest: useFCV40}));

        // Can't movePrimary for 'local' database.
        assert.commandFailed(
            mongos.adminCommand({movePrimary: 'local', to: shard0, forTest: useFCV40}));

        // Can't movePrimary for 'admin' database.
        assert.commandFailed(
            mongos.adminCommand({movePrimary: 'admin', to: shard0, forTest: useFCV40}));

        // Can't movePrimary for invalid db name.
        assert.commandFailed(
            mongos.adminCommand({movePrimary: 'a.b', to: shard0, forTest: useFCV40}));
        assert.commandFailed(mongos.adminCommand({movePrimary: '', to: shard0, forTest: useFCV40}));

        // Fail if 'to' shard does not exist or empty.
        assert.commandFailed(
            mongos.adminCommand({movePrimary: kDbName, to: 'Unknown', forTest: useFCV40}));
        assert.commandFailed(
            mongos.adminCommand({movePrimary: kDbName, to: '', forTest: useFCV40}));
        assert.commandFailed(mongos.adminCommand({movePrimary: kDbName, forTest: useFCV40}));

        // Succeed if 'to' shard exists and verify metadata changes.
        assert.eq(shard0, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);
        assert.commandWorked(
            mongos.adminCommand({movePrimary: kDbName, to: shard1, forTest: useFCV40}));
        assert.eq(shard1, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);

        // Succeed if 'to' shard is already the primary shard for the db.
        assert.commandWorked(
            mongos.adminCommand({movePrimary: kDbName, to: shard1, forTest: useFCV40}));
        assert.eq(shard1, mongos.getDB('config').databases.findOne({_id: kDbName}).primary);

        st.stop();
    }

    movePrimary(false);
    movePrimary(true);
})();
