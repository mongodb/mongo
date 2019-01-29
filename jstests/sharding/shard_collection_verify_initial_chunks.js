/**
 * Verify initial chunks are properly created and distributed in various combinations of shard key
 * and empty/non-empty collections.
 */
(function() {
    'use strict';

    let st = new ShardingTest({mongos: 1, shards: 2});
    let mongos = st.s0;

    let config = mongos.getDB("config");
    let db = mongos.getDB('TestDB');

    assert.commandWorked(mongos.adminCommand({enableSharding: 'TestDB'}));
    st.ensurePrimaryShard('TestDB', st.shard1.shardName);

    function checkChunkCounts(collName, chunksOnShard0, chunksOnShard1) {
        let counts = st.chunkCounts(collName, 'TestDB');
        assert.eq(chunksOnShard0,
                  counts[st.shard0.shardName],
                  'Count mismatch on shard0: ' + tojson(counts));
        assert.eq(chunksOnShard1,
                  counts[st.shard1.shardName],
                  'Count mismatch on shard1: ' + tojson(counts));
    }

    // Unsupported: Range sharding + numInitialChunks
    assert.commandFailed(mongos.adminCommand(
        {shardCollection: 'TestDB.RangeCollEmpty', key: {aKey: 1}, numInitialChunks: 6}));

    // Unsupported: Hashed sharding + numInitialChunks + non-empty collection
    assert.writeOK(db.HashedCollNotEmpty.insert({aKey: 1}));
    assert.commandWorked(db.HashedCollNotEmpty.createIndex({aKey: "hashed"}));
    assert.commandFailed(mongos.adminCommand({
        shardCollection: 'TestDB.HashedCollNotEmpty',
        key: {aKey: "hashed"},
        numInitialChunks: 6
    }));

    // Supported: Hashed sharding + numInitialChunks + empty collection
    // Expected: Even chunk distribution
    assert.commandWorked(db.HashedCollEmpty.createIndex({aKey: "hashed"}));
    assert.commandWorked(mongos.adminCommand(
        {shardCollection: 'TestDB.HashedCollEmpty', key: {aKey: "hashed"}, numInitialChunks: 6}));
    checkChunkCounts('HashedCollEmpty', 3, 3);

    // Supported: Hashed sharding + numInitialChunks + non-existent collection
    // Expected: Even chunk distribution
    assert.commandWorked(mongos.adminCommand({
        shardCollection: 'TestDB.HashedCollNonExistent',
        key: {aKey: "hashed"},
        numInitialChunks: 6
    }));
    checkChunkCounts('HashedCollNonExistent', 3, 3);

    st.stop();
})();
