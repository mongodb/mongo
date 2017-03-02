/*
 * Test to ensure that (dump/restore/export/import/oplog) works with a replica set connection string
 * 1. Start a replica set.
 * 2. Add data to a collection.
 * 3. Take a dump of the database.
 * 4. Drop the db.
 * 5. Restore the db.
 * 6. Export a collection.
 * 7. Drop the collection.
 * 8. Import the collection.
 * 9. Add data to the oplog.rs collection.
 * 10. Ensure that the document doesn't exist yet.
 * 11. Now play the bongooplog tool.
 * 12. Make sure that the oplog was played
*/

(function() {
    "use strict";

    var replTest = new ReplSetTest({name: 'tool_replset', nodes: 2, oplogSize: 5});
    var nodes = replTest.startSet();
    var config = replTest.getReplSetConfig();
    config.members[0].priority = 3;
    config.members[1].priority = 0;
    replTest.initiate(config);
    var master = replTest.getPrimary();
    assert.eq(nodes[0], master, "incorrect master elected");
    for (var i = 0; i < 100; i++) {
        assert.writeOK(master.getDB("foo").bar.insert({a: i}));
    }
    replTest.awaitReplication();

    var replSetConnString =
        "tool_replset/127.0.0.1:" + replTest.ports[0] + ",127.0.0.1:" + replTest.ports[1];

    // Test with bongodump/bongorestore
    print("dump the db");
    var data = BongoRunner.dataDir + "/tool_replset-dump1/";
    var exitCode = BongoRunner.runBongoTool("bongodump", {
        host: replSetConnString,
        out: data,
    });
    assert.eq(0, exitCode, "bongodump failed to dump from the replica set");

    print("db successfully dumped, dropping now");
    master.getDB("foo").dropDatabase();
    replTest.awaitReplication();

    print("restore the db");
    exitCode = BongoRunner.runBongoTool("bongorestore", {
        host: replSetConnString,
        dir: data,
    });
    assert.eq(0, exitCode, "bongorestore failed to restore data to the replica set");

    print("db successfully restored, checking count");
    var x = master.getDB("foo").getCollection("bar").count();
    assert.eq(x, 100, "bongorestore should have successfully restored the collection");

    replTest.awaitReplication();

    // Test with bongoexport/bongoimport
    print("export the collection");
    var extFile = BongoRunner.dataDir + "/tool_replset/export";
    exitCode = BongoRunner.runBongoTool("bongoexport", {
        host: replSetConnString,
        out: extFile,
        db: "foo",
        collection: "bar",
    });
    assert.eq(
        0, exitCode, "bongoexport failed to export collection 'foo.bar' from the replica set");

    print("collection successfully exported, dropping now");
    master.getDB("foo").getCollection("bar").drop();
    replTest.awaitReplication();

    print("import the collection");
    exitCode = BongoRunner.runBongoTool("bongoimport", {
        host: replSetConnString,
        file: extFile,
        db: "foo",
        collection: "bar",
    });
    assert.eq(
        0, exitCode, "bongoimport failed to import collection 'foo.bar' into the replica set");

    var x = master.getDB("foo").getCollection("bar").count();
    assert.eq(x, 100, "bongoimport should have successfully imported the collection");
    var doc = {_id: 5, x: 17};
    var oplogEntry = {ts: new Timestamp(), "op": "i", "ns": "foo.bar", "o": doc, "v": NumberInt(2)};
    assert.writeOK(master.getDB("local").oplog.rs.insert(oplogEntry));

    assert.eq(100,
              master.getDB("foo").getCollection("bar").count(),
              "count before running " + "bongooplog was not 100 as expected");

    exitCode = BongoRunner.runBongoTool("bongooplog", {
        from: "127.0.0.1:" + replTest.ports[0],
        host: replSetConnString,
    });
    assert.eq(0, exitCode, "bongooplog failed to replay the oplog");

    print("finished running bongooplog to replay the oplog");

    assert.eq(101,
              master.getDB("foo").getCollection("bar").count(),
              "count after running " + "bongooplog was not 101 as expected");

    print("all tests successful, stopping replica set");

    replTest.stopSet();

    print("replica set stopped, test complete");
}());
