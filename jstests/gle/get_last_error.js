// Check that the wtime and writtenTo fields are set or unset depending on the writeConcern used.
// First check on a replica set with different combinations of writeConcern
var name = "SERVER-9005";
var replTest = new ReplSetTest( {name: name, oplogSize: 1, nodes: 3} );
var nodes = replTest.startSet();
replTest.initiate({
    _id: name,
    members: [
        { _id: 0, host: replTest.nodeList()[0] },
        { _id: 1, host: replTest.nodeList()[1] },
        { _id: 2, host: replTest.nodeList()[2] }
    ],
    settings: { chainingAllowed: false }
});
var master = replTest.getMaster();
var mdb = master.getDB("test");

// synchronize replication
mdb.foo.insert({ _id: "1" });
replTest.awaitReplication();

// do a second write to do gle tests on
mdb.foo.insert({ _id: "2" });

var gle = master.getDB("test").runCommand({getLastError : 1, j : true});
print('Trying j=true');
printjson(gle);
if (gle.err === null) {
    assert.eq(gle.ok, 1);
    assert.eq(gle.writtenTo, null);
    assert.eq(gle.waited, null);
    assert.eq(gle.wtime, null);
    assert.eq(gle.wtimeout, null);
}
else {
    // Bad GLE is a permissible error here, if journaling is disabled.
    assert(gle.badGLE);
    assert.eq(gle.code, 2);
}

gle = mdb.getLastErrorObj(1, 10);
print('Trying w=1, 10ms timeout');
printjson(gle);
assert.eq(gle.ok, 1);
assert.eq(gle.err, null);
assert.eq(gle.writtenTo, null);
assert.eq(gle.wtime, null);
assert.eq(gle.waited, null);
assert.eq(gle.wtimeout, null);

gle = mdb.getLastErrorObj(3, 2000);
print('Trying w=3, 2000ms timeout.');
printjson(gle);
assert.eq(gle.ok, 1);
assert.eq(gle.err, null);
assert.eq(gle.writtenTo.length, 3);
assert.gte(gle.wtime, 0);
assert.eq(gle.waited, null);
assert.eq(gle.wtimeout, null);

// take a node down and GLE for more nodes than are up
replTest.stop(2);
master = replTest.getMaster();
mdb = master.getDB("test");
mdb.foo.insert({_id: "3"});
gle = mdb.getLastErrorObj(3, 1000);
print('Trying w=3 with 2 nodes up, 1000ms timeout.');
printjson(gle);
assert.eq(gle.ok, 1);
assert.eq(gle.err, "timeout");
assert.eq(gle.writtenTo.length, 2);
assert.eq(gle.wtime, null);
assert.gte(gle.waited, 5);
assert.eq(gle.wtimeout, true);

gle = mdb.getLastErrorObj("majority", 5);
print('Trying w=majority, 5ms timeout.');
printjson(gle);
assert.eq(gle.ok, 1);
assert.eq(gle.err, null);
assert.eq(gle.writtenTo.length, 2);
assert.eq(gle.wtime, 0);
assert.eq(gle.waited, null);
assert.eq(gle.wtimeout, null);

replTest.stopSet();

// Next check that it still works on a lone mongod
// Need to start a single server manually to keep this test in the jstests/replsets test suite
var port = allocatePorts(1)[0];
var baseName = "SERVER-9005";
var mongod = startMongod("--port", port, "--dbpath", MongoRunner.dataPath + baseName);
var sdb = new Mongo("localhost:"+port).getDB("test");

sdb.foo.drop();
sdb.foo.insert({ _id: "1" });

gle = sdb.getLastErrorObj(1);
print('Trying standalone server with w=1.');
printjson(gle);
assert.eq(gle.ok, 1);
assert.eq(gle.err, null);
assert.eq(gle.writtenTo, null);
assert.eq(gle.wtime, null);
assert.eq(gle.waited, null);
assert.eq(gle.wtimeout, null);

gle = sdb.runCommand({getLastError : 1, w : 2, wtimeout : 10 });
print('Trying standalone server with w=2 and 10ms timeout.');
// This is an error in 2.6
printjson(gle);
assert.eq(gle.ok, 0);
assert(gle.badGLE);

stopMongod(port);
