(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, chunkSize: 1});

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.user', key: {x: 'hashed'}}));

    var configDB = st.s0.getDB('config');
    var chunkCountBefore = configDB.chunks.count({ns: 'test.user'});
    assert.gt(chunkCountBefore, 1);

    var testDB = st.s0.getDB('test');
    for (var x = 0; x < 1000; x++) {
        testDB.user.insert({x: x});
    }

    var chunkDoc = configDB.chunks.find({ns: 'test.user'}).sort({min: 1}).next();
    var min = chunkDoc.min;
    var max = chunkDoc.max;

    // Assumption: There are documents in the MinKey chunk, otherwise, splitVector will fail.
    //
    // Note: This chunk will have 267 documents if collection was presplit to 4.
    var cmdRes =
        assert.commandWorked(st.s0.adminCommand({split: 'test.user', bounds: [min, max]}),
                             'Split on bounds failed for chunk [' + tojson(chunkDoc) + ']');

    chunkDoc = configDB.chunks.find({ns: 'test.user'}).sort({min: 1}).skip(1).next();

    var middle = NumberLong(chunkDoc.min.x + 1000000);
    cmdRes = assert.commandWorked(st.s0.adminCommand({split: 'test.user', middle: {x: middle}}),
                                  'Split failed with middle [' + middle + ']');

    cmdRes = assert.commandWorked(st.s0.adminCommand({split: 'test.user', find: {x: 7}}),
                                  'Split failed with find.');

    var chunkList = configDB.chunks.find({ns: 'test.user'}).sort({min: 1}).toArray();
    assert.eq(chunkCountBefore + 3, chunkList.length);

    chunkList.forEach(function(chunkToMove) {
        var toShard = configDB.shards.findOne({_id: {$ne: chunkToMove.shard}})._id;

        print('Moving chunk ' + chunkToMove._id + ' from shard ' + chunkToMove.shard + ' to ' +
              toShard + ' ...');

        assert.commandWorked(st.s0.adminCommand({
            moveChunk: 'test.user',
            bounds: [chunkToMove.min, chunkToMove.max],
            to: toShard,
            _waitForDelete: true
        }));
    });

    st.stop();
})();
