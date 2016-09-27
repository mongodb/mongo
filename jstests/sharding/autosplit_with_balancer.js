(function() {

    var s = new ShardingTest({name: "auto2", shards: 2, mongos: 2, other: {enableAutoSplit: true}});

    s.adminCommand({enablesharding: "test"});
    s.ensurePrimaryShard('test', 'shard0001');
    s.adminCommand({shardcollection: "test.foo", key: {num: 1}});

    var bigString = "";
    while (bigString.length < 1024 * 50) {
        bigString += "asocsancdnsjfnsdnfsjdhfasdfasdfasdfnsadofnsadlkfnsaldknfsad";
    }

    var db = s.getDB("test");
    var coll = db.foo;

    var i = 0;
    for (var j = 0; j < 30; j++) {
        print("j:" + j + " : " + Date.timeFunc(function() {
            var bulk = coll.initializeUnorderedBulkOp();
            for (var k = 0; k < 100; k++) {
                bulk.insert({num: i, s: bigString});
                i++;
            }
            assert.writeOK(bulk.execute());
        }));
    }

    s.startBalancer();

    assert.eq(i, j * 100, "setup");

    // Until SERVER-9715 is fixed, the sync command must be run on a diff connection
    new Mongo(s.s.host).adminCommand("connpoolsync");

    print("done inserting data");

    print("datasize: " +
          tojson(s.getPrimaryShard("test").getDB("admin").runCommand({datasize: "test.foo"})));
    s.printChunks();

    function doCountsGlobal() {
        counta = s._connections[0].getDB("test").foo.count();
        countb = s._connections[1].getDB("test").foo.count();
        return counta + countb;
    }

    // Wait for the chunks to distribute
    assert.soon(function() {
        doCountsGlobal();
        print("Counts: " + counta + countb);

        return counta > 0 && countb > 0;
    });

    print("checkpoint B");

    var missing = [];

    for (i = 0; i < j * 100; i++) {
        var x = coll.findOne({num: i});
        if (!x) {
            missing.push(i);
            print("can't find: " + i);
            sleep(5000);
            x = coll.findOne({num: i});
            if (!x) {
                print("still can't find: " + i);

                for (var zzz = 0; zzz < s._connections.length; zzz++) {
                    if (s._connections[zzz].getDB("test").foo.findOne({num: i})) {
                        print("found on wrong server: " + s._connections[zzz]);
                    }
                }
            }
        }
    }

    s.printChangeLog();

    print("missing: " + tojson(missing));
    assert.soon(function(z) {
        return doCountsGlobal() == j * 100;
    }, "from each a:" + counta + " b:" + countb + " i:" + i);
    print("checkpoint B.a");
    s.printChunks();
    assert.eq(j * 100, coll.find().limit(100000000).itcount(), "itcount A");
    assert.eq(j * 100, counta + countb, "from each 2 a:" + counta + " b:" + countb + " i:" + i);
    assert(missing.length == 0, "missing : " + tojson(missing));

    print("checkpoint C");

    assert(Array.unique(s.config.chunks.find().toArray().map(function(z) {
                    return z.shard;
                })).length == 2,
           "should be using both servers");

    for (i = 0; i < 100; i++) {
        cursor = coll.find().batchSize(5);
        cursor.next();
        cursor.close();
    }

    print("checkpoint D");

    // test not-sharded cursors
    db = s.getDB("test2");
    t = db.foobar;
    for (i = 0; i < 100; i++)
        t.save({_id: i});
    for (i = 0; i < 100; i++) {
        var cursor = t.find().batchSize(2);
        cursor.next();
        assert.lt(0, db.serverStatus().metrics.cursor.open.total, "cursor1");
        cursor.close();
    }

    assert.eq(0, db.serverStatus().metrics.cursor.open.total, "cursor2");

    // Stop the balancer, otherwise it may grab some connections from the pool for itself
    s.stopBalancer();

    print("checkpoint E");

    assert(t.findOne(), "check close 0");

    for (i = 0; i < 20; i++) {
        var conn = new Mongo(db.getMongo().host);
        temp2 = conn.getDB("test2").foobar;
        assert.eq(conn._fullNameSpace, t._fullNameSpace, "check close 1");
        assert(temp2.findOne(), "check close 2");
        conn.close();
    }

    print("checkpoint F");

    assert.throws(function() {
        s.getDB("test").foo.find().sort({s: 1}).forEach(function(x) {
            printjsononeline(x.substring(0, x.length > 30 ? 30 : x.length));
        });
    });

    print("checkpoint G");

    s.stop();

})();
