// should check that election happens in priority order

import {ReplSetTest} from "jstests/libs/replsettest.js";

let replTest = new ReplSetTest({name: "testSet", nodes: 3});
let nodenames = replTest.nodeList();

let nodes = replTest.startSet();
replTest.initiate(
    {
        "_id": "testSet",
        "members": [
            {"_id": 0, "host": nodenames[0], "priority": 1},
            {"_id": 1, "host": nodenames[1], "priority": 2},
            {"_id": 2, "host": nodenames[2], "priority": 3},
        ],
    },
    null,
    {initiateWithDefaultElectionTimeout: true},
);

// 2 should be primary (give this a while to happen, as other nodes might first be elected)
replTest.awaitNodesAgreeOnPrimary(replTest.timeoutMS, nodes, nodes[2]);

// wait for 1 to not appear to be primary (we are about to make it primary and need a clean slate
// here)
replTest.awaitSecondaryNodes(null, [nodes[1]]);

// Wait for election oplog entry to be replicated, to ensure 0 will vote for 1 after stopping 2.
replTest.awaitReplication();

// kill 2, 1 should take over
replTest.stop(2);

// 1 should eventually be primary
replTest.waitForState(nodes[1], ReplSetTest.State.PRIMARY);

// do some writes on 1
let primary = replTest.getPrimary();
for (var i = 0; i < 1000; i++) {
    assert.commandWorked(primary.getDB("foo").bar.insert({i: i}, {writeConcern: {w: "majority"}}));
}

for (i = 0; i < 1000; i++) {
    assert.commandWorked(primary.getDB("bar").baz.insert({i: i}, {writeConcern: {w: "majority"}}));
}

// bring 2 back up, 2 should wait until caught up and then become primary
replTest.restart(2);
replTest.awaitNodesAgreeOnPrimary(replTest.timeoutMS, nodes, nodes[2]);

// make sure nothing was rolled back
primary = replTest.getPrimary();
for (i = 0; i < 1000; i++) {
    assert(primary.getDB("foo").bar.findOne({i: i}) != null, "checking " + i);
    assert(primary.getDB("bar").baz.findOne({i: i}) != null, "checking " + i);
}
replTest.stopSet();
