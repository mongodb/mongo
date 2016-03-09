(function() {

    var s = new ShardingTest({
        name: "multcollections",
        shards: 2,
        mongos: 1,
        verbose: 1,
        other: {chunkSize: 1, enableBalancer: true}
    });

    s.adminCommand({enablesharding: "test"});
    db = s.getDB("test");
    s.ensurePrimaryShard('test', 'shard0001');

    N = 100000;

    S = "";
    while (S.length < 500)
        S += "123123312312";

    var bulk = db.foo.initializeUnorderedBulkOp();
    var bulk2 = db.bar.initializeUnorderedBulkOp();
    for (i = 0; i < N; i++) {
        bulk.insert({_id: i, s: S});
        bulk2.insert({_id: i, s: S, s2: S});
    }
    assert.writeOK(bulk.execute());
    assert.writeOK(bulk2.execute());

    s.printShardingStatus();

    function mytest(coll, i, loopNumber) {
        x = coll.find({_id: i}).explain();
        if (x)
            return;
        throw Error("can't find " + i + " in " + coll.getName() + " on loopNumber: " + loopNumber +
                    " explain: " + tojson(x));
    }

    loopNumber = 0;
    while (1) {
        for (i = 0; i < N; i++) {
            mytest(db.foo, i, loopNumber);
            mytest(db.bar, i, loopNumber);
            if (i % 1000 == 0)
                print(i);
        }
        s.printShardingStatus();
        loopNumber++;

        if (loopNumber == 1) {
            s.adminCommand({shardcollection: "test.foo", key: {_id: 1}});
            s.adminCommand({shardcollection: "test.bar", key: {_id: 1}});
        }

        assert(loopNumber < 1000, "taking too long");

        if (s.chunkDiff("foo") < 12 && s.chunkDiff("bar") < 12)
            break;
    }

    s.stop();

})();
