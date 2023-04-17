// Requires no shards.
// @tags: [config_shard_incompatible]
(function() {
'use strict';

var st = new ShardingTest({shards: 2});
var config = st.s.getDB('config');
var admin = st.s.getDB('admin');

jsTest.log('Cannot movePrimary on the config database');
{
    // At first, there should not be an entry for config
    assert.eq(0, config.databases.count({"_id": "config"}));

    // Test that we can enable sharding on the config db
    assert.commandWorked(admin.runCommand({enableSharding: 'config'}));

    // We should never have a metadata doc for config, it is generated in-mem
    assert.eq(0, config.databases.count({"_id": "config"}));

    // Test that you cannot set the primary shard for config (not even to 'config')
    assert.commandFailed(admin.runCommand({movePrimary: 'config', to: st.shard0.shardName}));
    assert.commandFailed(admin.runCommand({movePrimary: 'config', to: 'config'}));
}

jsTest.log('Only system.sessions may be sharded');
{
    assert.commandWorked(
        admin.runCommand({shardCollection: "config.system.sessions", key: {_id: 1}}));
    assert.eq(0, st.s.getDB('config').chunks.count({"shard": "config"}));

    assert.commandFailed(admin.runCommand({shardCollection: "config.anythingelse", key: {_id: 1}}));
}

st.stop();

{
    var st = new ShardingTest({shards: 0});
    var admin = st.s.getDB('admin');

    assert.commandFailed(
        admin.runCommand({shardCollection: "config.system.sessions", key: {_id: 1}}));

    st.stop();
}
})();
