/**
 * This test creates a replica set and  writes during the resync call in order to verify
 * that all phases of the resync/initial-sync process work correctly.
 *
 */
var testName = "resync_with_write_load";
var replTest = new ReplSetTest({name: testName, nodes: 3, oplogSize: 100});
var nodes = replTest.nodeList();

var conns = replTest.startSet();
var config = {
    "_id": testName,
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1], priority: 0},
        {"_id": 2, "host": nodes[2], priority: 0}
    ],
    settings: {chainingAllowed: false}
};
var r = replTest.initiate(config);
replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);
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
assert.writeOK(A.foo.insert({x: 1}, {writeConcern: {w: 3, wtimeout: 60000}}));

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

print("*************** issuing resync command ***************");
assert.commandWorked(B.adminCommand("resync"));

print("waiting for load generation to finish");
loadGen();

// Make sure oplogs & dbHashes match
replTest.checkOplogs(testName);
replTest.checkReplicatedDataHashes(testName);

replTest.stopSet();

print("*****test done******");
