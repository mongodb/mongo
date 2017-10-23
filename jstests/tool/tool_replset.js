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
*/

(function() {
    "use strict";

    var replTest =
        new ReplSetTest({name: 'tool_replset', nodes: 2, oplogSize: 5, nodeOptions: {"vvvvv": ""}});
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

    // Test with mongodump/mongorestore
    var data = MongoRunner.dataDir + "/tool_replset-dump1/";
    print("using mongodump to dump the db to " + data);
    var exitCode = MongoRunner.runMongoTool("mongodump", {
        host: replSetConnString,
        out: data,
    });
    assert.eq(0, exitCode, "mongodump failed to dump from the replica set");

    print("db successfully dumped to " + data +
          ". dropping collection before testing the restore process");
    assert(master.getDB("foo").bar.drop());
    replTest.awaitReplication();

    print("using mongorestore to restore the db from " + data);
    exitCode = MongoRunner.runMongoTool("mongorestore", {
        host: replSetConnString,
        dir: data,
    });
    assert.eq(0, exitCode, "mongorestore failed to restore data to the replica set");

    print("db successfully restored, checking count");
    var x = master.getDB("foo").getCollection("bar").count();
    assert.eq(x, 100, "mongorestore should have successfully restored the collection");

    replTest.awaitReplication();

    // Test with mongoexport/mongoimport
    print("export the collection");
    var extFile = MongoRunner.dataDir + "/tool_replset/export";
    exitCode = MongoRunner.runMongoTool("mongoexport", {
        host: replSetConnString,
        out: extFile,
        db: "foo",
        collection: "bar",
    });
    assert.eq(
        0, exitCode, "mongoexport failed to export collection 'foo.bar' from the replica set");

    print("collection successfully exported, dropping now");
    master.getDB("foo").getCollection("bar").drop();
    replTest.awaitReplication();

    print("import the collection");
    exitCode = MongoRunner.runMongoTool("mongoimport", {
        host: replSetConnString,
        file: extFile,
        db: "foo",
        collection: "bar",
    });
    assert.eq(
        0, exitCode, "mongoimport failed to import collection 'foo.bar' into the replica set");

    var x = master.getDB("foo").getCollection("bar").count();
    assert.eq(x, 100, "mongoimport should have successfully imported the collection");

    print("all tests successful, stopping replica set");

    replTest.stopSet();

    print("replica set stopped, test complete");
}());
