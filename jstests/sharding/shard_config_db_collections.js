(function() {
    'use strict';

    function assertNoChunksOnConfig() {
        assert.eq(0, config.chunks.count({"shard": "config"}));
    }

    // Database-level tests
    {
        var st = new ShardingTest({shards: 2});
        var config = st.s.getDB('config');
        var admin = st.s.getDB('admin');

        // At first, there should not be an entry for config
        assert.eq(0, config.databases.count({"_id": "config"}));

        // Test that we can enable sharding on the config db
        assert.commandWorked(admin.runCommand({enableSharding: 'config'}));

        // We should never have a metadata doc for config, it is generated in-mem
        assert.eq(0, config.databases.count({"_id": "config"}));

        // Test that you cannot set the primary shard for config (not even to 'config')
        assert.commandFailed(admin.runCommand({movePrimary: 'config', to: 'shard0000'}));
        assert.commandFailed(admin.runCommand({movePrimary: 'config', to: 'config'}));

        st.stop();
    }

    // Collection-level tests
    {
        var st = new ShardingTest({shards: 2});
        var config = st.s.getDB('config');
        var admin = st.s.getDB('admin');

        // Test that we can shard an empty collection in the config db
        assert.commandWorked(admin.runCommand({shardCollection: 'config.sharded', key: {_id: 1}}));
        assertNoChunksOnConfig();

        // Test that we cannot shard a collection in 'config' that holds data
        assert.writeOK(config.withdata.insert({a: 1}));
        assert.commandFailed(admin.runCommand({shardCollection: 'config.withdata', key: {_id: 1}}));

        // Test that we cannot specify an initial number of chunks,
        // because our shard key is not hashed.
        assert.commandFailed(admin.runCommand(
            {shardCollection: 'config.numchunks', key: {_id: 1}, numInitialChunks: 10}));
        assertNoChunksOnConfig();

        // Test that we cannot re-shard the same collection
        assert.commandFailed(admin.runCommand({shardCollection: 'config.sharded', key: {a: 1}}));
        assertNoChunksOnConfig();

        // Test that we can drop a sharded collection in config
        assert.commandWorked(admin.runCommand({shardCollection: 'config.todrop', key: {_id: 1}}));
        config.todrop.insert({a: 1});
        assertNoChunksOnConfig();
        assert(config.todrop.drop());
        assert.eq(0, config.chunks.count({"ns": "config.todrop"}));
        assert.eq(0, config.todrop.count());

        // Test that we can drop a non-sharded collection in config
        config.newcoll.insert({a: 1});
        assert(config.newcoll.drop());
        assert.eq(0, config.newcoll.count());
        assertNoChunksOnConfig();

        st.stop();
    }

    // CRUD operations on sharded collections in config
    {
        var st = new ShardingTest({shards: 2});
        var config = st.s.getDB('config');
        var admin = st.s.getDB('admin');

        assert.commandWorked(admin.runCommand({shardCollection: 'config.sharded', key: {_id: 1}}));
        assertNoChunksOnConfig();

        // Insertion and retrieval
        assert.commandWorked(st.splitAt("config.sharded", {_id: 0}));
        assert.commandWorked(
            admin.runCommand({moveChunk: "config.sharded", find: {_id: -10}, to: "shard0000"}));
        assert.commandWorked(
            admin.runCommand({moveChunk: "config.sharded", find: {_id: 10}, to: "shard0001"}));

        assert.writeOK(config.sharded.insert({_id: -10}));
        assert.writeOK(config.sharded.insert({_id: 10}));
        assert.eq({_id: -10}, config.sharded.findOne({_id: -10}));
        assert.eq({_id: 10}, config.sharded.findOne({_id: 10}));

        var shard0000 = st._connections[0].getDB("config");
        var shard0001 = st._connections[1].getDB("config");
        assert.eq({_id: -10}, shard0000.sharded.findOne());
        assert.eq({_id: 10}, shard0001.sharded.findOne());

        assert.eq(2, config.sharded.count());

        for (var i = 0; i < 100; i++) {
            assert.writeOK(config.sharded.insert({a: i}));
        }

        // Updates
        assert.writeOK(config.sharded.update({_id: 10}, {$set: {a: 15}}));
        assert.writeOK(config.sharded.update({_id: 10}, {$set: {a: 20}}));

        // Deletes
        assert.writeOK(config.sharded.remove({_id: 10}));
        assert.writeOK(config.sharded.remove({a: {$gt: 50}}));

        // Make an index
        assert.commandWorked(config.sharded.createIndex({a: 1}));

        st.stop();
    }

    // Chunk-level operations
    {
        var st = new ShardingTest({shards: 2});
        var config = st.s.getDB('config');
        var admin = st.s.getDB('admin');

        assert.commandWorked(admin.runCommand({shardCollection: 'config.sharded', key: {_id: 1}}));
        assertNoChunksOnConfig();

        // Test that we can split chunks
        assert.commandWorked(st.splitAt("config.sharded", {_id: 10}));
        assert.commandWorked(st.splitAt("config.sharded", {_id: 100}));
        assert.commandWorked(st.splitAt("config.sharded", {_id: 1000}));
        assertNoChunksOnConfig();

        // Try to move a chunk to config shard, should fail
        assert.commandFailed(
            admin.runCommand({moveChunk: "config.sharded", find: {_id: 40}, to: "config"}));
        assertNoChunksOnConfig();

        // Test that we can move chunks between two non-config shards
        assert.commandWorked(
            admin.runCommand({moveChunk: "config.sharded", find: {_id: 40}, to: "shard0001"}));
        assert.commandWorked(
            admin.runCommand({moveChunk: "config.sharded", find: {_id: 40}, to: "shard0000"}));
        assertNoChunksOnConfig();

        st.stop();
    }

    // When test commands are not enabled, only system.sessions may be sharded.
    {
        jsTest.setOption('enableTestCommands', false);

        var st = new ShardingTest({shards: 2});
        var admin = st.s.getDB('admin');

        assert.commandWorked(
            admin.runCommand({shardCollection: "config.system.sessions", key: {_id: 1}}));
        assert.commandFailed(
            admin.runCommand({shardCollection: "config.anythingelse", key: {_id: 1}}));

        st.stop();
    }

    // Cannot shard things in config without shards.
    {
        var st = new ShardingTest({shards: 0});
        var admin = st.s.getDB('admin');

        assert.commandFailed(
            admin.runCommand({shardCollection: "config.system.sessions", key: {_id: 1}}));

        st.stop();
    }

})();
