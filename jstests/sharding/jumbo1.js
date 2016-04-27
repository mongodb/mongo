(function() {

    var s = new ShardingTest({name: "jumbo1", shards: 2, mongos: 1, other: {chunkSize: 1}});

    s.adminCommand({enablesharding: "test"});
    s.ensurePrimaryShard('test', 'shard0001');
    s.adminCommand({shardcollection: "test.foo", key: {x: 1}});

    db = s.getDB("test");

    big = "";
    while (big.length < 10000)
        big += ".";

    x = 0;
    var bulk = db.foo.initializeUnorderedBulkOp();
    for (; x < 500; x++)
        bulk.insert({x: x, big: big});

    for (i = 0; i < 500; i++)
        bulk.insert({x: x, big: big});

    for (; x < 2000; x++)
        bulk.insert({x: x, big: big});

    assert.writeOK(bulk.execute());

    s.printShardingStatus(true);

    res = sh.moveChunk("test.foo", {x: 0}, "shard0001");
    if (!res.ok)
        res = sh.moveChunk("test.foo", {x: 0}, "shard0000");

    s.printShardingStatus(true);

    sh.setBalancerState(true);

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
