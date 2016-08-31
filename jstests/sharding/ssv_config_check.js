/**
 * Test that setShardVersion should not reject a configdb string with the same
 * replica set name, but with a member list that is not strictly the same.
 */
(function() {
    "use strict";

    var st = new ShardingTest({shards: 1});

    var testDB = st.s.getDB('test');
    testDB.adminCommand({enableSharding: 'test'});
    testDB.adminCommand({shardCollection: 'test.user', key: {x: 1}});

    // Initialize version on shard.
    testDB.user.insert({x: 1});

    var directConn = new Mongo(st.d0.host);
    var adminDB = directConn.getDB('admin');

    var configStr = adminDB.runCommand({getShardVersion: 'test.user'}).configServer;
    var alternateConfigStr = configStr.substring(0, configStr.lastIndexOf(','));

    var shardDoc = st.s.getDB('config').shards.findOne();

    assert.commandWorked(adminDB.runCommand({
        setShardVersion: '',
        init: true,
        authoritative: true,
        configdb: alternateConfigStr,
        shard: shardDoc._id,
        shardHost: shardDoc.host
    }));

    assert.commandFailed(adminDB.runCommand({
        setShardVersion: '',
        init: true,
        authoritative: true,
        configdb: 'bad-rs/local:12,local:34',
        shard: shardDoc._id,
        shardHost: shardDoc.host
    }));

    st.stop();
})();
