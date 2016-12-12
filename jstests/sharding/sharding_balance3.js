// Waits for the balancer to run once, then stops it and checks that it is no longer running.

(function() {

    var s = new ShardingTest({
        name: "slow_sharding_balance3",
        shards: 2,
        mongos: 1,
        other: {chunkSize: 1, enableBalancer: true}
    });

    s.adminCommand({enablesharding: "test"});
    s.ensurePrimaryShard('test', 'shard0001');

    s.config.settings.find().forEach(printjson);

    db = s.getDB("test");

    bigString = "";
    while (bigString.length < 10000)
        bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";

    inserted = 0;
    num = 0;
    var bulk = db.foo.initializeUnorderedBulkOp();
    while (inserted < (40 * 1024 * 1024)) {
        bulk.insert({_id: num++, s: bigString});
        inserted += bigString.length;
    }
    assert.writeOK(bulk.execute());

    s.adminCommand({shardcollection: "test.foo", key: {_id: 1}});
    assert.lt(20, s.config.chunks.count(), "setup2");

    function diff1() {
        var x = s.chunkCounts("foo");
        printjson(x);
        return Math.max(x.shard0000, x.shard0001) - Math.min(x.shard0000, x.shard0001);
    }

    assert.lt(10, diff1());

    // Wait for balancer to kick in.
    var initialDiff = diff1();
    assert.soon(function() {
        return diff1() != initialDiff;
    }, "Balancer did not kick in", 5 * 60 * 1000, 1000);

    print("* A");
    print("disabling the balancer");
    s.stopBalancer();
    s.config.settings.find().forEach(printjson);
    print("* B");

    print(diff1());

    var currDiff = diff1();
    var waitTime = 0;
    var startTime = Date.now();
    while (waitTime < (1000 * 60)) {
        // Wait for 60 seconds to ensure balancer did not run
        assert.eq(currDiff, diff1(), "balance with stopped flag should not have happened");
        sleep(5000);
        waitTime = Date.now() - startTime;
    }

    s.stop();

})();
