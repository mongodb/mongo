(function() {

    var st = new ShardingTest({shards: 2});

    var configDB = st.s.getDB('config');
    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));

    st.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(configDB.adminCommand(
        {shardCollection: 'test.user', key: {x: 'hashed'}, numInitialChunks: 2}));

    var metadata =
        st.d0.getDB('admin').runCommand({getShardVersion: 'test.user', fullMetadata: true});
    var chunks =
        metadata.metadata.chunks.length > 0 ? metadata.metadata.chunks : metadata.metadata.pending;
    assert(bsonWoCompare(chunks[0][0], chunks[0][1]) < 0, tojson(metadata));

    metadata = st.d1.getDB('admin').runCommand({getShardVersion: 'test.user', fullMetadata: true});
    chunks =
        metadata.metadata.chunks.length > 0 ? metadata.metadata.chunks : metadata.metadata.pending;
    assert(bsonWoCompare(chunks[0][0], chunks[0][1]) < 0, tojson(metadata));

    st.stop();

})();
