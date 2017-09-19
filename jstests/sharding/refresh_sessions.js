(function() {
    "use strict";

    var sessionsDb = "config";
    var refresh = {refreshLogicalSessionCacheNow: 1};
    var startSession = {startSession: 1};

    // Create a cluster with 1 shard.
    var cluster = new ShardingTest({shards: 2});

    // Test that we can refresh without any sessions, as a sanity check.
    {
        assert.commandWorked(cluster.s.getDB(sessionsDb).runCommand(refresh));
        assert.commandWorked(cluster.shard0.getDB(sessionsDb).runCommand(refresh));
        assert.commandWorked(cluster.shard1.getDB(sessionsDb).runCommand(refresh));
    }

    // Test that refreshing on mongos flushes local records to the collection.
    {
        var mongos = cluster.s.getDB(sessionsDb);
        var sessionCount = mongos.system.sessions.count();

        // Start one session.
        assert.commandWorked(mongos.runCommand(startSession));
        assert.commandWorked(mongos.runCommand(refresh));

        // Test that it landed in the collection.
        assert.eq(mongos.system.sessions.count(),
                  sessionCount + 1,
                  "refresh on mongos did not flush session record");
    }

    // Test that refreshing on mongod flushes local records to the collection.
    {
        var mongos = cluster.s.getDB(sessionsDb);
        var shard = cluster.shard0.getDB(sessionsDb);
        var sessionCount = mongos.system.sessions.count();

        assert.commandWorked(shard.runCommand(startSession));
        assert.commandWorked(shard.runCommand(refresh));

        // Test that the new record landed in the collection.
        assert.eq(mongos.system.sessions.count(),
                  sessionCount + 1,
                  "refresh on mongod did not flush session record");
    }

    // Test that refreshing on all servers flushes all records.
    {
        var mongos = cluster.s.getDB(sessionsDb);
        var shard0 = cluster.shard0.getDB(sessionsDb);
        var shard1 = cluster.shard1.getDB(sessionsDb);

        var sessionCount = mongos.system.sessions.count();

        assert.commandWorked(mongos.runCommand(startSession));
        assert.commandWorked(shard0.runCommand(startSession));
        assert.commandWorked(shard1.runCommand(startSession));

        // All records should be in local caches only.
        assert.eq(mongos.system.sessions.count(),
                  sessionCount,
                  "startSession should not flush records to disk");

        // Refresh on each server, see that it ups the session count.
        assert.commandWorked(mongos.runCommand(refresh));
        assert.eq(mongos.system.sessions.count(),
                  sessionCount + 1,
                  "refresh on mongos did not flush session records to disk");

        assert.commandWorked(shard0.runCommand(refresh));
        assert.eq(mongos.system.sessions.count(),
                  sessionCount + 2,
                  "refresh on shard did not flush session records to disk");

        assert.commandWorked(shard1.runCommand(refresh));
        assert.eq(mongos.system.sessions.count(),
                  sessionCount + 3,
                  "refresh on shard did not flush session records to disk");
    }
})();
