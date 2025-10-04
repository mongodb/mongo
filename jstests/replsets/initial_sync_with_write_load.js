/**
 * This test creates replica set and then puts load on the primary with writes during
 * the initial sync in order to verify that all phases of the initial sync work correctly.
 *
 * We cannot test each phase of the initial sync directly but by providing constant writes we can
 * assume that each individual phase will have data to work with, and therefore be tested.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

let testName = "initialsync_with_write_load";
let replTest = new ReplSetTest({name: testName, nodes: 3, oplogSize: 100});
let nodes = replTest.nodeList();

let conns = replTest.startSet();
let config = {
    "_id": testName,
    "members": [
        {"_id": 0, "host": nodes[0], priority: 4},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2]},
    ],
};
let r = replTest.initiate(config);
replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);
// Make sure we have a primary
let primary = replTest.getPrimary();
let a_conn = conns[0];
let b_conn = conns[1];
a_conn.setSecondaryOk();
b_conn.setSecondaryOk();
let A = a_conn.getDB("test");
let B = b_conn.getDB("test");
let AID = replTest.getNodeId(a_conn);
let BID = replTest.getNodeId(b_conn);

assert(primary == conns[0], "conns[0] assumed to be primary");
assert(a_conn.host == primary.host);

// create an oplog entry with an insert
assert.commandWorked(A.foo.insert({x: 1}, {writeConcern: {w: 1, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));
replTest.stop(BID);

print("******************** starting load for 30 secs *********************");
let work = function () {
    print("starting loadgen");
    let start = new Date().getTime();

    assert.commandWorked(db.timeToStartTrigger.insert({_id: 1}));

    while (true) {
        for (let x = 0; x < 100; x++) {
            db["a" + x].insert({a: x});
        }

        let runTime = new Date().getTime() - start;
        if (runTime > 30000) break;
        else if (runTime < 5000)
            // back-off more during first 2 seconds
            sleep(50);
        else sleep(1);
    }
    print("finishing loadgen");
};
// insert enough that resync node has to go through oplog replay in each step
let loadGen = startParallelShell(work, replTest.ports[0]);

// wait for document to appear to continue
assert.soon(
    function () {
        try {
            return 1 == primary.getDB("test")["timeToStartTrigger"].find().itcount();
        } catch (e) {
            print(e);
            return false;
        }
    },
    "waited too long for start trigger",
    90 * 1000 /* 90 secs */,
);

print("*************** STARTING node without data ***************");
replTest.start(BID);
// check that it is up
assert.soon(function () {
    try {
        let result = b_conn.getDB("admin").runCommand({replSetGetStatus: 1});
        return true;
    } catch (e) {
        print(e);
        return false;
    }
}, "node didn't come up");

print("waiting for load generation to finish");
loadGen();

// Wait for initial sync to finish.
replTest.awaitSecondaryNodes();

// Make sure oplogs & dbHashes match
replTest.checkOplogs(testName);
replTest.checkReplicatedDataHashes(testName);

replTest.stopSet();

print("*****test done******");
