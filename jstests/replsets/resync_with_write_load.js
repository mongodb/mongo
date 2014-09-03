/**
 * This test creates a 2 node replica set and then puts load on the primary with writes during 
 * the resync in order to verify that all phases of the initial sync work correctly.
 * 
 * We cannot test each phase of the initial sync directly but by providing constant writes we can 
 * assume that each individual phase will have data to work with, and therefore tested.
 */
var replTest = new ReplSetTest({name: 'resync', nodes: 2, oplogSize: 100});
var nodes = replTest.nodeList();

var conns = replTest.startSet();
var config = { "_id": "resync",
               "members": [
                            {"_id": 0, "host": nodes[0], priority:4},
                            {"_id": 1, "host": nodes[1]}]
              };
var r = replTest.initiate(config);

// Make sure we have a master
var master = replTest.getMaster();
var a_conn = conns[0];
var b_conn = conns[1];
a_conn.setSlaveOk();
b_conn.setSlaveOk();
var A = a_conn.getDB("test");
var B = b_conn.getDB("test");
var AID = replTest.getNodeId(a_conn);
var BID = replTest.getNodeId(b_conn);
assert(master == conns[0], "conns[0] assumed to be master");
assert(a_conn.host == master.host);

// create an oplog entry with an insert
assert.writeOK( A.foo.insert({ x: 1 }, { writeConcern: { w: 1, wtimeout: 60000 }}));
replTest.stop(BID);

print("******************** starting load for 30 secs *********************");
var work = 'var start=new Date().getTime(); db.timeToStartTrigger.insert({_id:1}); while(true) {for(x=0;x<1000;x++) {db["a" + x].insert({a:x})};sleep(1); if((new Date().getTime() - start) > 30000) break; }';

//insert enough that resync node has to go through oplog replay in each step
var loadGen = startParallelShell( work, replTest.ports[0] );

// wait for document to appear to continue
assert.soon(function() {
    try {
        return 1 == a_conn.getDB("test")["timeToStartTrigger"].count();
    } catch ( e ) {
        print( e );
        return false;
    }
}, "waited too long for start trigger");

print("*************** STARTING node without data ***************");
replTest.start(BID);
// check that it is up
assert.soon(function() {
    try {
        var result = b_conn.getDB("admin").runCommand({replSetGetStatus: 1});
        return true;
    } catch ( e ) {
        print( e );
        return false;
    }
}, "node didn't come up");

print("waiting for load generation to finish");
loadGen();

// load must stop before we await replication.
replTest.awaitReplication();

// Make sure oplogs match
try {
    replTest.ensureOplogsMatch();
} catch (e) {
    var aDBHash = A.runCommand("dbhash");
    var bDBHash = B.runCommand("dbhash");
    assert.eq(aDBHash.md5, bDBHash.md5, 
              "hashes differ: " + tojson(aDBHash) + " to " + tojson(bDBHash));
}
replTest.stopSet();

print("*****test done******");
