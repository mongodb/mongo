(function() {
    'use strict';

    var s = new ShardingTest({shards: 2, mongos: 1, other: {chunkSize: 1}});

    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    s.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {x: 1}}));

    var db = s.getDB("test");

    var big = "";
    while (big.length < 10000) {
        big += ".";
    }

    var x = 0;
    var bulk = db.foo.initializeUnorderedBulkOp();
    for (; x < 500; x++) {
        bulk.insert({x: x, big: big});
    }

    for (var i = 0; i < 500; i++) {
        bulk.insert({x: x, big: big});
    }

    for (; x < 2000; x++) {
        bulk.insert({x: x, big: big});
    }

    assert.writeOK(bulk.execute());

    s.printShardingStatus(true);
    assert.commandWorked(s.s0.adminCommand({moveChunk: 'test.foo', find: {x: 0}, to: 'shard0000'}));
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

    s.stop();

})();
