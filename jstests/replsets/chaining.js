// Test that chaining position propegation (percolate) still works properly 
// in the face of socket exceptions

function assertGLEOK(status) {
    assert(status.ok && status.err === null,
           "Expected OK status object; found " + tojson(status));
}

var replTest = new ReplSetTest({name: 'testSet', nodes: 3});
var nodes = replTest.startSet();
var hostnames = replTest.nodeList();
replTest.initiate(
    {
        "_id" : "testSet",
        "members" : [
            {"_id" : 0, "host" : hostnames[0], "priority" : 2},
            {"_id" : 1, "host" : hostnames[1]},
            {"_id" : 2, "host" : hostnames[2]}
        ],
    }
);

replTest.awaitReplication();

replTest.bridge();
replTest.partition(0, 2);

// Now 0 and 2 can't see each other, so 2 should chain through 1 to reach 0.

var master = replTest.getMaster();
var cdb = master.getDB("chaining");
var admin = nodes[1].getDB("admin");
cdb.foo.insert({a:1});
assertGLEOK(cdb.getLastErrorObj());
replTest.awaitReplication();
print("1")
var result = admin.runCommand( { configureFailPoint: 'rsChaining1', mode: { times : 1 } } );
assert.eq(1, result.ok, 'rsChaining1');
cdb.foo.insert({a:1});
sleep(1);
assertGLEOK(cdb.getLastErrorObj());
replTest.awaitReplication();
print("2")
admin.runCommand( { configureFailPoint: 'rsChaining2', mode: { times : 1 } } );
assert.eq(1, result.ok, 'rsChaining2');
cdb.foo.insert({a:1});
sleep(1);
assertGLEOK(cdb.getLastErrorObj());
replTest.awaitReplication();
print("3")
admin.runCommand( { configureFailPoint: 'rsChaining3', mode: { times : 1 } } );
assert.eq(1, result.ok, 'rsChaining3');
cdb.foo.insert({a:1});
sleep(1);
assertGLEOK(cdb.getLastErrorObj());
replTest.awaitReplication();
replTest.stopSet();
