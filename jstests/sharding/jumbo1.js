(function() {
    'use strict';

    var s = new ShardingTest({shards: 2, mongos: 1, other: {chunkSize: 1, enableAutoSplit: true}});

    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    s.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {x: 1}}));

    var db = s.getDB("test");

    var big = "";
    while (big.length < 10000) {
        big += ".";
    }

    // Create sufficient documents to create a jumbo chunk, and use the same shard key in all of
    // them so that the chunk cannot be split.
    var x = 0;
    var bulk = db.foo.initializeUnorderedBulkOp();
    for (var i = 0; i < 500; i++) {
        bulk.insert({x: x, big: big});
    }

    // Create documents with different shard keys that can be split and moved without issue.
    for (; x < 1500; x++) {
        bulk.insert({x: x, big: big});
    }

    assert.writeOK(bulk.execute());

    s.printShardingStatus(true);

    s.startBalancer();

    function diff1() {
        var x = s.chunkCounts("foo");
        printjson(x);
        return Math.max(x.shard0000, x.shard0001) - Math.min(x.shard0000, x.shard0001);
    }

    assert.soon(function() {
        var d = diff1();
        print("diff: " + d);
        s.printShardingStatus(true);
        return d < 5;
    }, "balance didn't happen", 1000 * 60 * 5, 5000);

    // Check that the jumbo chunk did not move, which shouldn't be possible.
    var jumboChunk =
        s.getDB('config').chunks.findOne({ns: 'test.foo', min: {$lte: {x: 0}}, max: {$gt: {x: 0}}});
    assert.eq('shard0001', jumboChunk.shard, 'jumbo chunk ' + tojson(jumboChunk) + ' was moved');
    // TODO: SERVER-26531 Make sure that balancer marked the first chunk as jumbo.
    // Assumption: balancer favors moving the lowest valued chunk out of a shard.
    // assert(jumboChunk.jumbo, tojson(jumboChunk));

    s.stop();

})();
