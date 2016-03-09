/**
 * Basic test for the 'sharding' section of the serverStatus response object for
 * both mongos and the shard.
 */

(function() {
    "use strict";

    var st = new ShardingTest({shards: 1});

    var testDB = st.s.getDB('test');
    testDB.adminCommand({enableSharding: 'test'});
    testDB.adminCommand({shardCollection: 'test.user', key: {_id: 1}});

    // Initialize shard metadata in shards
    testDB.user.insert({x: 1});

    var checkShardingServerStatus = function(doc, isCSRS) {
        var shardingSection = doc.sharding;
        assert.neq(shardingSection, null);

        var configConnStr = shardingSection.configsvrConnectionString;
        var configConn = new Mongo(configConnStr);
        var configIsMaster = configConn.getDB('admin').runCommand({isMaster: 1});

        var configOpTimeObj = shardingSection.lastSeenConfigServerOpTime;

        if (isCSRS) {
            assert.gt(configConnStr.indexOf('/'), 0);
            assert.eq(1, configIsMaster.configsvr);  // If it's a shard, this field won't exist.
            assert.neq(null, configOpTimeObj);
            assert.neq(null, configOpTimeObj.ts);
            assert.neq(null, configOpTimeObj.t);
        } else {
            assert.eq(-1, configConnStr.indexOf('/'));
            assert.gt(configConnStr.indexOf(','), 0);
            assert.eq(0, configIsMaster.configsvr);
            assert.eq(null, configOpTimeObj);
        }
    };

    var mongosServerStatus = testDB.adminCommand({serverStatus: 1});
    var isCSRS = st.configRS != null;
    checkShardingServerStatus(mongosServerStatus, isCSRS);

    var mongodServerStatus = st.d0.getDB('admin').runCommand({serverStatus: 1});
    checkShardingServerStatus(mongodServerStatus, isCSRS);

    st.stop();
})();
