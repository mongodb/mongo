// Check that the wtime and writtenTo fields are set or unset depending on the writeConcern used.
// First check on a replica set with different combinations of writeConcern
var replTest = new ReplSetTest( {name: "SERVER-9005", oplogSize: 1, nodes: 2} );
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();
var mdb = master.getDB("test");

// synchronize replication
mdb.foo.insert({ _id: "1" });
replTest.awaitReplication();

// do a second write to do gle tests on
mdb.foo.insert({ _id: "2" });
var gle = master.getDB("test").runCommand({getLastError : 1, j : true, wtimeout : 60000});

print('Trying j=true, 60000ms timeout');
printjson(gle);
assert.eq(gle.err, null);
assert.eq(gle.writtenTo, null);
assert.eq(gle.waited, null);
assert.eq(gle.wtime, null);
assert.eq(gle.wtimeout, null);

gle = mdb.getLastErrorObj(1, 10);
print('Trying w=1, 10ms timeout');
printjson(gle);
assert.eq(gle.err, null);
assert.eq(gle.writtenTo, null);
assert.eq(gle.wtime, null);
assert.eq(gle.waited, null);
assert.eq(gle.wtimeout, null);

gle = mdb.getLastErrorObj(2, 2000);
print('Trying w=2, 2000ms timeout.');
printjson(gle);
assert.eq(gle.err, null);
assert.eq(gle.writtenTo.length, 2);
assert.gte(gle.wtime, 0);
assert.eq(gle.waited, null);
assert.eq(gle.wtimeout, null);

// only two members in the set, this must fail fast
gle = mdb.getLastErrorObj(3, 5);
print('Trying w=3, 5ms timeout.  Should timeout.');
printjson(gle);
assert.eq(gle.err, "timeout");
assert.eq(gle.writtenTo.length, 2);
assert.eq(gle.wtime, null);
assert.gte(gle.waited, 5);
assert.eq(gle.wtimeout, true);

gle = mdb.getLastErrorObj("majority", 5);
print('Trying w=majority, 5ms timeout.');
printjson(gle);
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
var mongod = startMongod("--port", port, "--dbpath", "/data/db/" + baseName);
var sdb = new Mongo("localhost:"+port).getDB("test");

sdb.foo.drop();
sdb.foo.insert({ _id: "1" });

gle = sdb.getLastErrorObj(1);
print('Trying standalone server with w=1.');
printjson(gle);
assert.eq(gle.err, null);
assert.eq(gle.writtenTo, null);
assert.eq(gle.wtime, null);
assert.eq(gle.waited, null);
assert.eq(gle.wtimeout, null);

gle = sdb.getLastErrorObj(2, 10);
print('Trying standalone server with w=2 and 10ms timeout.');
printjson(gle);
assert.eq(gle.err, "norepl");
assert.eq(gle.writtenTo, null);
assert.eq(gle.wtime, null);
assert.eq(gle.waited, null);
assert.eq(gle.wtimeout, null);

stopMongod(port);
