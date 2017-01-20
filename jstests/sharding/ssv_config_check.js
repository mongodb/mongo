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

    testDB.user.insert({x: 1});

    var directConn = new Mongo(st.d0.host);
    var adminDB = directConn.getDB('admin');

    var configStr = adminDB.runCommand({getShardVersion: 'test.user'}).configServer;
    var alternateConfigStr = configStr.substring(0, configStr.lastIndexOf(','));

    var shardDoc = st.s.getDB('config').shards.findOne();

    jsTest.log("Verify that the obsolete init form of setShardVersion succeeds on shards.");
    assert.commandWorked(adminDB.runCommand({
        setShardVersion: '',
        init: true,
        authoritative: true,
        configdb: alternateConfigStr,
        shard: shardDoc._id,
        shardHost: shardDoc.host
    }));

    var configAdmin = st.c0.getDB('admin');

    jsTest.log("Verify that setShardVersion fails on the config server");
    // Even if shardName sent is 'config' and connstring sent is config server's actual connstring.
    assert.commandFailedWithCode(configAdmin.runCommand({
        setShardVersion: '',
        init: true,
        authoritative: true,
        configdb: configStr,
        shard: 'config'
    }),
                                 ErrorCodes.NoShardingEnabled);

    st.stop();
})();
