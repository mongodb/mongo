// This test is skipped on 32-bit platforms

function setupTest() {
    var s = new ShardingTest({
        shards: 2,
        mongos: 1,
        other: {
            rs: true,
            numReplicas: 2,
            chunkSize: 1,
            rsOptions: {oplogSize: 50},
            enableBalancer: true
        }
    });

    // Reduce chunk size to split
    var config = s.getDB("config");

    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    s.ensurePrimaryShard('test', 'test-rs0');
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {"_id": 1}}));
    return s;
}

function runTest(s) {
    jsTest.log("Inserting a lot of documents into test.foo");
    db = s.getDB("test");

    var idInc = 0;
    var valInc = 0;
    var str = "";

    if (db.serverBuildInfo().bits == 32) {
        // Make data ~0.5MB for 32 bit builds
        for (var i = 0; i < 512; i++)
            str += "a";
    } else {
        // Make data ~4MB
        for (var i = 0; i < 4 * 1024; i++)
            str += "a";
    }

    var bulk = db.foo.initializeUnorderedBulkOp();
    for (j = 0; j < 100; j++) {
        for (i = 0; i < 512; i++) {
            bulk.insert({i: idInc++, val: valInc++, y: str});
        }
    }
    assert.writeOK(bulk.execute({w: 2, wtimeout: 10 * 60 * 1000}));

    jsTest.log("Documents inserted, doing double-checks of insert...");

    // Collect some useful stats to figure out what happened
    if (db.foo.find().itcount() != 51200) {
        sleep(1000);

        s.printShardingStatus(true);

        print("Shard 0: " + s.shard0.getCollection(db.foo + "").find().itcount());
        print("Shard 1: " + s.shard1.getCollection(db.foo + "").find().itcount());

        for (var i = 0; i < 51200; i++) {
            if (!db.foo.findOne({i: i}, {i: 1})) {
                print("Could not find: " + i);
            }

            if (i % 100 == 0)
                print("Checked " + i);
        }

        print("PROBABLY WILL ASSERT NOW");
    }

    assert.soon(function() {
        var c = db.foo.find().itcount();
        if (c == 51200) {
            return true;
        }

        print("Count is " + c);
        return false;
    });

    s.printChunks();
    s.printChangeLog();

    function map() {
        emit('count', 1);
    }
    function reduce(key, values) {
        return Array.sum(values);
    }

    jsTest.log("Test basic mapreduce...");

    // Test basic mapReduce
    for (var iter = 0; iter < 5; iter++) {
        print("Test #" + iter);
        out = db.foo.mapReduce(map, reduce, "big_out");
    }

    print("Testing output to different db...");

    // test output to a different DB
    // do it multiple times so that primary shard changes
    for (iter = 0; iter < 5; iter++) {
        print("Test #" + iter);

        assert.eq(51200, db.foo.find().itcount(), "Not all data was found!");

        outCollStr = "mr_replace_col_" + iter;
        outDbStr = "mr_db_" + iter;

        print("Testing mr replace into DB " + iter);

        res = db.foo.mapReduce(map, reduce, {out: {replace: outCollStr, db: outDbStr}});
        printjson(res);

        outDb = s.getDB(outDbStr);
        outColl = outDb[outCollStr];

        obj = outColl.convertToSingleObject("value");

        assert.eq(51200, obj.count, "Received wrong result " + obj.count);

        print("checking result field");
        assert.eq(res.result.collection, outCollStr, "Wrong collection " + res.result.collection);
        assert.eq(res.result.db, outDbStr, "Wrong db " + res.result.db);
    }

    jsTest.log("Verifying nonatomic M/R throws...");

    // check nonAtomic output
    assert.throws(function() {
        db.foo.mapReduce(map, reduce, {out: {replace: "big_out", nonAtomic: true}});
    });

    jsTest.log();

    // Add docs with dup "i"
    valInc = 0;
    for (j = 0; j < 100; j++) {
        print("Inserted document: " + (j * 100));
        bulk = db.foo.initializeUnorderedBulkOp();
        for (i = 0; i < 512; i++) {
            bulk.insert({i: idInc++, val: valInc++, y: str});
        }
        // wait for replication to catch up
        assert.writeOK(bulk.execute({w: 2, wtimeout: 10 * 60 * 1000}));
    }

    jsTest.log("No errors...");

    map2 = function() {
        emit(this.val, 1);
    };
    reduce2 = function(key, values) {
        return Array.sum(values);
    };

    // Test merge
    outcol = "big_out_merge";

    // M/R quarter of the docs
    jsTestLog("Test A");
    out = db.foo.mapReduce(map2, reduce2, {query: {i: {$lt: 25600}}, out: {merge: outcol}});
    printjson(out);
    assert.eq(25600, out.counts.emit, "Received wrong result");
    assert.eq(25600, out.counts.output, "Received wrong result");

    // M/R further docs
    jsTestLog("Test B");
    out = db.foo.mapReduce(
        map2, reduce2, {query: {i: {$gte: 25600, $lt: 51200}}, out: {merge: outcol}});
    printjson(out);
    assert.eq(25600, out.counts.emit, "Received wrong result");
    assert.eq(51200, out.counts.output, "Received wrong result");

    // M/R do 2nd half of docs
    jsTestLog("Test C");
    out = db.foo.mapReduce(
        map2, reduce2, {query: {i: {$gte: 51200}}, out: {merge: outcol, nonAtomic: true}});
    printjson(out);
    assert.eq(51200, out.counts.emit, "Received wrong result");
    assert.eq(51200, out.counts.output, "Received wrong result");
    assert.eq(1, db[outcol].findOne().value, "Received wrong result");

    // Test reduce
    jsTestLog("Test D");
    outcol = "big_out_reduce";

    // M/R quarter of the docs
    out = db.foo.mapReduce(map2, reduce2, {query: {i: {$lt: 25600}}, out: {reduce: outcol}});
    printjson(out);
    assert.eq(25600, out.counts.emit, "Received wrong result");
    assert.eq(25600, out.counts.output, "Received wrong result");

    // M/R further docs
    jsTestLog("Test E");
    out = db.foo.mapReduce(
        map2, reduce2, {query: {i: {$gte: 25600, $lt: 51200}}, out: {reduce: outcol}});
    printjson(out);
    assert.eq(25600, out.counts.emit, "Received wrong result");
    assert.eq(51200, out.counts.output, "Received wrong result");

    // M/R do 2nd half of docs
    jsTestLog("Test F");
    out = db.foo.mapReduce(
        map2, reduce2, {query: {i: {$gte: 51200}}, out: {reduce: outcol, nonAtomic: true}});
    printjson(out);
    assert.eq(51200, out.counts.emit, "Received wrong result");
    assert.eq(51200, out.counts.output, "Received wrong result");
    assert.eq(2, db[outcol].findOne().value, "Received wrong result");

    // Verify that data is also on secondary
    jsTestLog("Test G");
    var primary = s._rs[0].test.liveNodes.master;
    var secondaries = s._rs[0].test.liveNodes.slaves;

    // Stop the balancer to prevent new writes from happening and make sure
    // that replication can keep up even on slow machines.
    s.stopBalancer();
    s._rs[0].test.awaitReplication();
    assert.eq(51200, primary.getDB("test")[outcol].count(), "Wrong count");
    for (var i = 0; i < secondaries.length; ++i) {
        assert.eq(51200, secondaries[i].getDB("test")[outcol].count(), "Wrong count");
    }
}

var s = setupTest();

if (s.getDB("admin").runCommand("buildInfo").bits < 64) {
    print("Skipping test on 32-bit platforms");
} else {
    runTest(s);
}

s.stop();
