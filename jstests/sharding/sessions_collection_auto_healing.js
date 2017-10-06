load('jstests/libs/sessions_collection.js');

(function() {
    "use strict";

    var st = new ShardingTest({shards: 0});
    var configSvr = st.configRS.getPrimary();
    var configAdmin = configSvr.getDB("admin");

    var mongos = st.s;
    var mongosAdmin = mongos.getDB("admin");
    var mongosConfig = mongos.getDB("config");

    // Test that we can use sessions on the config server before we add any shards.
    {
        validateSessionsCollection(configSvr, false, false);
        validateSessionsCollection(mongos, false, false);

        assert.commandWorked(configAdmin.runCommand({startSession: 1}));

        validateSessionsCollection(configSvr, false, false);
        validateSessionsCollection(mongos, false, false);
    }

    // Test that we can use sessions on a mongos before we add any shards.
    {
        validateSessionsCollection(configSvr, false, false);
        validateSessionsCollection(mongos, false, false);

        assert.commandWorked(mongosAdmin.runCommand({startSession: 1}));

        validateSessionsCollection(configSvr, false, false);
        validateSessionsCollection(mongos, false, false);
    }

    // Test that the config server does not create the sessions collection
    // if there are not any shards.
    {
        assert.eq(mongosConfig.shards.count(), 0);

        assert.commandWorked(configAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

        validateSessionsCollection(configSvr, false, false);
    }

    // Test-wide: add a shard
    var rs = new ReplSetTest({nodes: 1});
    rs.startSet({shardsvr: ""});
    rs.initiate();

    var shard = rs.getPrimary();
    var shardAdmin = shard.getDB("admin");
    var shardConfig = shard.getDB("config");

    // Test that we can add this shard, even with a local config.system.sessions collection,
    // and test that we drop its local collection
    {
        shardConfig.system.sessions.insert({"hey": "you"});
        validateSessionsCollection(shard, true, false);

        assert.commandWorked(mongosAdmin.runCommand({addShard: rs.getURL()}));
        assert.eq(mongosConfig.shards.count(), 1);
        validateSessionsCollection(shard, false, false);
    }

    // Test that we can use sessions on a shard before the sessions collection
    // is set up by the config servers.
    {
        validateSessionsCollection(configSvr, false, false);
        validateSessionsCollection(shard, false, false);

        assert.commandWorked(shardAdmin.runCommand({startSession: 1}));

        validateSessionsCollection(configSvr, false, false);
        validateSessionsCollection(shard, false, false);
    }

    // Test that we can use sessions from a mongos before the sessions collection
    // is set up by the config servers.
    {
        validateSessionsCollection(configSvr, false, false);
        validateSessionsCollection(shard, false, false);
        validateSessionsCollection(mongos, false, false);

        assert.commandWorked(mongosAdmin.runCommand({startSession: 1}));

        validateSessionsCollection(configSvr, false, false);
        validateSessionsCollection(shard, false, false);
        validateSessionsCollection(mongos, false, false);
    }

    // Test that if we do a refresh (write) from a shard server while there
    // is no sessions collection, it does not create the sessions collection.
    {
        validateSessionsCollection(configSvr, false, false);
        validateSessionsCollection(shard, false, false);

        assert.commandWorked(shardAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

        validateSessionsCollection(configSvr, false, false);
        validateSessionsCollection(shard, false, false);
    }

    // Test that a refresh on the config servers once there are shards creates
    // the sessions collection on a shard.
    {
        validateSessionsCollection(shard, false, false);

        assert.commandWorked(configAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

        validateSessionsCollection(shard, true, true);

        assert.eq(shardConfig.system.sessions.count(), 1, "did not flush config's sessions");

        // Now, if we do refreshes on the other servers, their in-mem records will
        // be written to the collection.
        assert.commandWorked(shardAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));
        assert.eq(shardConfig.system.sessions.count(), 2, "did not flush shard's sessions");

        assert.commandWorked(mongosAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));
        assert.eq(shardConfig.system.sessions.count(), 4, "did not flush mongos' sessions");
    }

    // Test that if we drop the index on the sessions collection,
    // refresh on neither the shard nor the config db heals it.
    {
        assert.commandWorked(shardConfig.system.sessions.dropIndex({lastUse: 1}));

        validateSessionsCollection(shard, true, false);

        assert.commandWorked(configAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));
        validateSessionsCollection(shard, true, false);

        assert.commandWorked(shardAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));
        validateSessionsCollection(shard, true, false);
    }

    st.stop();

})();
