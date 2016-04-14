(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, chunkSize: 1});

    var testDB = st.s.getDB('test');
    assert.commandWorked(testDB.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(testDB.adminCommand({shardCollection: 'test.user', key: {x: 'hashed'}}));

    var configDB = st.s.getDB('config');
    var chunkCountBefore = configDB.chunks.count();
    assert.gt(chunkCountBefore, 1);

    for (var x = 0; x < 1000; x++) {
        testDB.user.insert({x: x});
    }

    var chunkDoc = configDB.chunks.find().sort({min: 1}).next();
    var min = chunkDoc.min;
    var max = chunkDoc.max;

    // Assumption: There are documents in the MinKey chunk, otherwise, splitVector will
    // fail. Note: This chunk will have 267 documents if collection was presplit to 4.
    var cmdRes = testDB.adminCommand({split: 'test.user', bounds: [min, max]});
    assert(cmdRes.ok,
           'split on bounds failed on chunk[' + tojson(chunkDoc) + ']: ' + tojson(cmdRes));

    chunkDoc = configDB.chunks.find().sort({min: 1}).skip(1).next();
    var middle = chunkDoc.min + 1000000;

    cmdRes = testDB.adminCommand({split: 'test.user', middle: {x: middle}});
    assert(cmdRes.ok, 'split failed with middle [' + middle + ']: ' + tojson(cmdRes));

    cmdRes = testDB.adminCommand({split: 'test.user', find: {x: 7}});
    assert(cmdRes.ok, 'split failed with find: ' + tojson(cmdRes));

    var chunkList = configDB.chunks.find().sort({min: 1}).toArray();
    assert.eq(chunkCountBefore + 3, chunkList.length);

    chunkList.forEach(function(chunkToMove) {
        var toShard = configDB.shards.findOne({_id: {$ne: chunkToMove.shard}})._id;

        print(jsTestName() + " - moving chunk " + chunkToMove._id + " from shard " +
              chunkToMove.shard + " to " + toShard + "...");

        var cmdRes = testDB.adminCommand({
            moveChunk: 'test.user',
            bounds: [chunkToMove.min, chunkToMove.max],
            to: toShard,
            _waitForDelete: true
        });
        print(jsTestName() + " - result from moving chunk " + chunkToMove._id + ": " +
              tojson(cmdRes));
    });

    st.stop();
})();
