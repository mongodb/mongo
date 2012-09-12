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
 * 11. Now play the mongooplog tool.
 * 12. Make sure that the oplog was played
*/

// Load utility methods for replica set tests
load("jstests/replsets/rslib.js");

var replTest = new ReplSetTest({ name: 'rs', nodes: 2, oplogSize: 5 });
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();
var docNum = 100;
for (var i = 0; i < docNum; i++) {
    master.getDB("foo").bar.insert({ a: i });
}
replTest.awaitReplication();

var replSetConnString = "rs/127.0.0.1:" + replTest.ports[0] + ",127.0.0.1:" + replTest.ports[1];

// Test with mongodump/mongorestore
print("dump the db");
var data = "/data/db/dumprestore11-dump1/";
runMongoProgram("mongodump", "--host", replSetConnString, "--out", data);

master.getDB("foo").dropDatabase();
replTest.awaitReplication();

print("restore the db");
runMongoProgram("mongorestore", "--host", replSetConnString, "--dir", data);

var x = master.getDB("foo").getCollection("bar").count();
assert.eq(x, docNum, "mongorestore should have successfully restored the collection" + docNum);

replTest.awaitReplication();

// Test with mongoexport/mongoimport
print("export the collection");
var extFile = "/data/db/exportimport_replSet/export";
runMongoProgram("mongoexport", "--host", replSetConnString, "--out", extFile,
                "-d", "foo", "-c", "bar");

master.getDB("foo").getCollection("bar").drop();
replTest.awaitReplication();

print("import the collection");
runMongoProgram("mongoimport", "--host", replSetConnString, "--file", extFile,
                "-d", "foo", "-c", "bar");

var x = master.getDB("foo").getCollection("bar").count();
assert.eq(x, docNum, "mongoimport should have successfully imported the collection" + docNum);

master.getDB("foo").getCollection("bar").drop();

// Test with mongooplog
var doc = { _id : 5, x : 17 };
master.getDB("local").oplog.rs.insert({ ts : new Timestamp(), "op" : "i", "ns" : "foo.bar",
                                         "o" : doc });

assert.eq(0, master.getDB("foo").getCollection("bar").count(), "before");

var replSetConnString = "rs/127.0.0.1:" + replTest.ports[0] + ",127.0.0.1:" + replTest.ports[1];
runMongoProgram("mongooplog" , "--from", "127.0.0.1:" + replTest.ports[0],
                               "--host", replSetConnString);

assert.eq(101, master.getDB("foo").getCollection("bar").count(), "after")

replTest.stopSet();
