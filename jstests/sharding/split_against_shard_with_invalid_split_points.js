// Tests that executing splitChunk directly against a shard, with an invalid split point will not
// corrupt the chunks metadata
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1});

    var testDB = st.s.getDB('TestSplitDB');
    assert.commandWorked(testDB.adminCommand({enableSharding: 'TestSplitDB'}));
    st.ensurePrimaryShard('TestSplitDB', st.shard0.shardName);

    assert.commandWorked(testDB.adminCommand({shardCollection: 'TestSplitDB.Coll', key: {x: 1}}));
    assert.commandWorked(testDB.adminCommand({split: 'TestSplitDB.Coll', middle: {x: 0}}));

    var chunksBefore = st.s.getDB('config').chunks.find().toArray();

    // Try to do a split with invalid parameters through mongod
    var callSplit = function(db, minKey, maxKey, splitPoints) {
        var res = assert.commandWorked(st.s.adminCommand({getShardVersion: 'TestSplitDB.Coll'}));
        var shardVersion = [res.version, res.versionEpoch];

        return db.runCommand({
            splitChunk: 'TestSplitDB.Coll',
            from: st.shard0.shardName,
            min: minKey,
            max: maxKey,
            keyPattern: {x: 1},
            splitKeys: splitPoints,
            shardVersion: shardVersion,
        });
    };

    assert.commandFailedWithCode(callSplit(st.d0.getDB('admin'), {x: MinKey}, {x: 0}, [{x: 2}]),
                                 ErrorCodes.InvalidOptions);

    var chunksAfter = st.s.getDB('config').chunks.find().toArray();
    assert.eq(chunksBefore,
              chunksAfter,
              'Split chunks failed, but the chunks were updated in the config database');

    st.stop();
})();
