// Test to make sure that tag ranges get split when partial keys are used for the tag ranges
(function() {
    'use strict';

    var s = new ShardingTest({shards: 2, mongos: 1});

    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    s.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {_id: 1, a: 1}}));

    assert.eq(1, s.config.chunks.find().itcount());

    s.addShardTag("shard0000", "a");
    s.addShardTag("shard0000", "b");

    s.addTagRange("test.foo", {_id: 5}, {_id: 10}, "a");
    s.addTagRange("test.foo", {_id: 10}, {_id: 15}, "b");

    s.startBalancer();

    assert.soon(function() {
        return s.config.chunks.find().itcount() == 4;
    }, 'Split did not occur', 3 * 60 * 1000);

    s.awaitBalancerRound();
    s.printShardingStatus(true);
    assert.eq(4, s.config.chunks.find().itcount(), 'Split points changed');

    s.config.chunks.find().forEach(function(chunk) {
        var numFields = 0;
        for (var x in chunk.min) {
            numFields++;
            assert(x == "_id" || x == "a", tojson(chunk));
        }
        assert.eq(2, numFields, tojson(chunk));
    });

    // Check chunk mins correspond exactly to tag range boundaries, extended to match shard key
    assert.eq(1, s.config.chunks.find({min: {_id: MinKey, a: MinKey}}).itcount());
    assert.eq(1, s.config.chunks.find({min: {_id: 5, a: MinKey}}).itcount());
    assert.eq(1, s.config.chunks.find({min: {_id: 10, a: MinKey}}).itcount());
    assert.eq(1, s.config.chunks.find({min: {_id: 15, a: MinKey}}).itcount());

    s.stop();
})();
