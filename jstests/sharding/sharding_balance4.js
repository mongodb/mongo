// Check that doing updates done during a migrate all go to the right place
(function() {

    var s = new ShardingTest(
        {name: "slow_sharding_balance4", shards: 2, mongos: 1, other: {chunkSize: 1}});

    s.adminCommand({enablesharding: "test"});
    s.ensurePrimaryShard('test', 'shard0001');
    s.adminCommand({shardcollection: "test.foo", key: {_id: 1}});
    assert.eq(1, s.config.chunks.count(), "setup1");

    s.config.settings.find().forEach(printjson);

    db = s.getDB("test");

    bigString = "";
    while (bigString.length < 10000)
        bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";

    N = 3000;

    num = 0;

    var counts = {};

    //
    // TODO: Rewrite to make much clearer.
    //
    // The core behavior of this test is to add a bunch of documents to a sharded collection, then
    // incrementally update each document and make sure the counts in the document match our update
    // counts while balancing occurs (doUpdate()).  Every once in a while we also check (check())
    // our counts via a query.
    //
    // If during a chunk migration an update is missed, we trigger an assertion and fail.
    //

    function doUpdate(bulk, includeString, optionalId) {
        var up = {$inc: {x: 1}};
        if (includeString) {
            up["$set"] = {s: bigString};
        }
        var myid = optionalId == undefined ? Random.randInt(N) : optionalId;
        bulk.find({_id: myid}).upsert().update(up);

        counts[myid] = (counts[myid] ? counts[myid] : 0) + 1;
        return myid;
    }

    Random.setRandomSeed();
    // Initially update all documents from 1 to N, otherwise later checks can fail because no
    // document
    // previously existed
    var bulk = db.foo.initializeUnorderedBulkOp();
    for (i = 0; i < N; i++) {
        doUpdate(bulk, true, i);
    }

    for (i = 0; i < N * 9; i++) {
        doUpdate(bulk, false);
    }
    assert.writeOK(bulk.execute());

    for (var i = 0; i < 50; i++) {
        s.printChunks("test.foo");
        if (check("initial:" + i, true))
            break;
        sleep(5000);
    }
    check("initial at end");

    assert.lt(20, s.config.chunks.count(), "setup2");

    function check(msg, dontAssert) {
        for (var x in counts) {
            var e = counts[x];
            var z = db.foo.findOne({_id: parseInt(x)});

            if (z && z.x == e)
                continue;

            if (dontAssert) {
                if (z)
                    delete z.s;
                print("not asserting for key failure: " + x + " want: " + e + " got: " + tojson(z));
                return false;
            }

            s.s.getDB("admin").runCommand({setParameter: 1, logLevel: 2});

            printjson(db.foo.findOne({_id: parseInt(x)}));

            var y = db.foo.findOne({_id: parseInt(x)});

            if (y) {
                delete y.s;
            }

            s.printChunks("test.foo");

            assert(z, "couldn't find : " + x + " y:" + tojson(y) + " e: " + e + " " + msg);
            assert.eq(e, z.x, "count for : " + x + " y:" + tojson(y) + " " + msg);
        }

        return true;
    }

    function diff1() {
        jsTest.log("Running diff1...");

        bulk = db.foo.initializeUnorderedBulkOp();
        var myid = doUpdate(bulk, false);
        var res = assert.writeOK(bulk.execute());

        assert.eq(1,
                  res.nModified,
                  "diff myid: " + myid + " 2: " + res.toString() + "\n" + " correct count is: " +
                      counts[myid] + " db says count is: " + tojson(db.foo.findOne({_id: myid})));

        var x = s.chunkCounts("foo");
        if (Math.random() > .999)
            printjson(x);
        return Math.max(x.shard0000, x.shard0001) - Math.min(x.shard0000, x.shard0001);
    }

    assert.lt(20, diff1(), "initial load");
    print(diff1());

    s.startBalancer();

    assert.soon(function() {
        var d = diff1();
        return d < 5;
    }, "balance didn't happen", 1000 * 60 * 20, 1);

    s.stop();

})();
