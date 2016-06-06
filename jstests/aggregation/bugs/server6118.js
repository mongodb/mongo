// SERVER-6118: support for sharded sorts
(function() {
    'use strict';

    var s = new ShardingTest({shards: 2});

    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    s.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.data", key: {_id: 1}}));

    var d = s.getDB("test");

    // Insert _id values 0 - 99
    var N = 100;

    var bulkOp = d.data.initializeOrderedBulkOp();
    for (var i = 0; i < N; ++i) {
        bulkOp.insert({_id: i});
    }
    bulkOp.execute();

    // Split the data into 3 chunks
    assert.commandWorked(s.s0.adminCommand({split: "test.data", middle: {_id: 33}}));
    assert.commandWorked(s.s0.adminCommand({split: "test.data", middle: {_id: 66}}));

    // Migrate the middle chunk to another shard
    assert.commandWorked(s.s0.adminCommand(
        {movechunk: "test.data", find: {_id: 50}, to: s.getOther(s.getPrimaryShard("test")).name}));

    // Check that the results are in order.
    var result = d.data.aggregate({$sort: {_id: 1}}).toArray();
    printjson(result);

    for (var i = 0; i < N; ++i) {
        assert.eq(i, result[i]._id);
    }

    s.stop();
})();
