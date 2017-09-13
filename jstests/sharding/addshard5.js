// Tests that dropping and re-adding a shard with the same name to a cluster doesn't mess up
// migrations
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, mongos: 1});

    var mongos = st.s;
    var admin = mongos.getDB('admin');
    var coll = mongos.getCollection('foo.bar');

    // Shard collection
    assert.commandWorked(mongos.adminCommand({enableSharding: coll.getDB() + ''}));

    // Just to be sure what primary we start from
    st.ensurePrimaryShard(coll.getDB().getName(), st.shard0.shardName);
    assert.commandWorked(mongos.adminCommand({shardCollection: coll + '', key: {_id: 1}}));

    // Insert one document
    assert.writeOK(coll.insert({hello: 'world'}));

    // Migrate the collection to and from shard1 so shard0 loads the shard1 host
    assert.commandWorked(mongos.adminCommand(
        {moveChunk: coll + '', find: {_id: 0}, to: st.shard1.shardName, _waitForDelete: true}));
    assert.commandWorked(mongos.adminCommand(
        {moveChunk: coll + '', find: {_id: 0}, to: st.shard0.shardName, _waitForDelete: true}));

    // Drop and re-add shard with the same name but a new host.
    assert.commandWorked(mongos.adminCommand({removeShard: st.shard1.shardName}));
    assert.commandWorked(mongos.adminCommand({removeShard: st.shard1.shardName}));

    var shard2 = MongoRunner.runMongod({'shardsvr': ''});
    assert.commandWorked(mongos.adminCommand({addShard: shard2.host, name: st.shard1.shardName}));

    jsTest.log('Shard was dropped and re-added with same name...');
    st.printShardingStatus();

    // Try a migration
    assert.commandWorked(
        mongos.adminCommand({moveChunk: coll + '', find: {_id: 0}, to: st.shard1.shardName}));

    assert.eq('world', shard2.getCollection(coll + '').findOne().hello);

    st.stop();
})();
