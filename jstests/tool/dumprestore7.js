(function() {
    "use strict";
    var name = "dumprestore7";

    var step = (function() {
        var n = 0;
        return function(msg) {
            msg = msg || "";
            print('\n' + name + ".js step " + (++n) + ' ' + msg);
        };
    })();

    step("starting the replset test");

    var replTest = new ReplSetTest({name: name, nodes: 1});
    var nodes = replTest.startSet();
    replTest.initiate();

    step("inserting first chunk of data");
    var foo = replTest.getPrimary().getDB("foo");
    for (var i = 0; i < 20; i++) {
        foo.bar.insert({x: i, y: "abc"});
    }

    step("waiting for replication");
    replTest.awaitReplication();
    assert.eq(foo.bar.count(), 20, "should have inserted 20 documents");

    // The time of the last oplog entry.
    var time = replTest.getPrimary()
                   .getDB("local")
                   .getCollection("oplog.rs")
                   .find()
                   .limit(1)
                   .sort({$natural: -1})
                   .next()
                   .ts;
    step("got time of last oplog entry: " + time);

    step("inserting second chunk of data");
    for (var i = 30; i < 50; i++) {
        foo.bar.insert({x: i, y: "abc"});
    }
    assert.eq(foo.bar.count(), 40, "should have inserted 40 total documents");

    step("try bongodump with $timestamp");

    var data = BongoRunner.dataDir + "/dumprestore7-dump1/";
    var query = {ts: {$gt: time}};
    print("bongodump query: " + tojson(query));

    var testQueryCount =
        replTest.getPrimary().getDB("local").getCollection("oplog.rs").find(query).itcount();
    assert.eq(testQueryCount, 20, "the query should match 20 documents");

    var exitCode = BongoRunner.runBongoTool("bongodump", {
        host: "127.0.0.1:" + replTest.ports[0],
        db: "local",
        collection: "oplog.rs",
        query: tojson(query),
        out: data,
    });
    assert.eq(0, exitCode, "monogdump failed to dump the oplog");

    step("try bongorestore from $timestamp");

    var restoreBongod = BongoRunner.runBongod({});
    exitCode = BongoRunner.runBongoTool("bongorestore", {
        host: "127.0.0.1:" + restoreBongod.port,
        dir: data,
        writeConcern: 1,
    });
    assert.eq(0, exitCode, "bongorestore failed to restore the oplog");

    var count = restoreBongod.getDB("local").getCollection("oplog.rs").count();
    if (count != 20) {
        print("bongorestore restored too many documents");
        restoreBongod.getDB("local").getCollection("oplog.rs").find().pretty().shellPrint();
        assert.eq(count, 20, "bongorestore should only have inserted the latter 20 entries");
    }

    step("stopping replset test");
    replTest.stopSet();
})();
