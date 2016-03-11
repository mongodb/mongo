/**
 * This test creates a 2 node replica set and then puts load on the primary with writes during
 * the resync in order to verify that all phases of the initial sync work correctly.
 *
 * We cannot test each phase of the initial sync directly but by providing constant writes we can
 * assume that each individual phase will have data to work with, and therefore tested.
 */
var testName = "resync_with_write_load";
var replTest = new ReplSetTest({name: testName, nodes: 3, oplogSize: 100});
var nodes = replTest.nodeList();

var conns = replTest.startSet();
var config = {
    "_id": testName,
    "members": [
        {"_id": 0, "host": nodes[0], priority: 4},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2]}
    ]
};
var r = replTest.initiate(config);
replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY, 60 * 1000);
// Make sure we have a master
var master = replTest.getPrimary();
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
assert.writeOK(A.foo.insert({x: 1}, {writeConcern: {w: 1, wtimeout: 60000}}));
replTest.stop(BID);

print("******************** starting load for 30 secs *********************");
var work = function() {
    print("starting loadgen");
    var start = new Date().getTime();

    assert.writeOK(db.timeToStartTrigger.insert({_id: 1}));

    while (true) {
        for (x = 0; x < 100; x++) {
            db["a" + x].insert({a: x});
        }

        var runTime = (new Date().getTime() - start);
        if (runTime > 30000)
            break;
        else if (runTime < 5000)  // back-off more during first 2 seconds
            sleep(50);
        else
            sleep(1);
    }
    print("finshing loadgen");
};
// insert enough that resync node has to go through oplog replay in each step
var loadGen = startParallelShell(work, replTest.ports[0]);

// wait for document to appear to continue
assert.soon(function() {
    try {
        return 1 == master.getDB("test")["timeToStartTrigger"].find().itcount();
    } catch (e) {
        print(e);
        return false;
    }
}, "waited too long for start trigger", 90 * 1000 /* 90 secs */);

print("*************** STARTING node without data ***************");
replTest.start(BID);
// check that it is up
assert.soon(function() {
    try {
        var result = b_conn.getDB("admin").runCommand({replSetGetStatus: 1});
        return true;
    } catch (e) {
        print(e);
        return false;
    }
}, "node didn't come up");

print("waiting for load generation to finish");
loadGen();

// load must stop before we await replication.
replTest.awaitReplication(240 * 1000);

// Make sure oplogs match
try {
    replTest.ensureOplogsMatch();
} catch (e) {
    var aDBHash = A.runCommand("dbhash");
    var bDBHash = B.runCommand("dbhash");
    assert.eq(
        aDBHash.md5, bDBHash.md5, "hashes differ: " + tojson(aDBHash) + " to " + tojson(bDBHash));
}
replTest.stopSet();

print("*****test done******");
