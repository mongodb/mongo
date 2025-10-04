// Tests that nodes sync from each other properly and that nodes find new sync sources when they
// are disconnected from their current sync source.

import {ReplSetTest} from "jstests/libs/replsettest.js";

let replTest = new ReplSetTest({
    name: "sync2",
    nodes: [{rsConfig: {priority: 5}}, {arbiter: true}, {}, {}, {}],
    useBridge: true,
});
let conns = replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();
jsTestLog("Replica set test initialized");

primary.getDB("foo").bar.insert({x: 1});
replTest.awaitReplication();

conns[0].disconnect(conns[4]);
conns[1].disconnect(conns[2]);
conns[2].disconnect(conns[3]);
conns[3].disconnect(conns[1]);

// 4 is connected to 2
conns[4].disconnect(conns[1]);
conns[4].disconnect(conns[3]);

assert.soon(
    function () {
        primary = replTest.getPrimary();
        return primary === conns[0];
    },
    "node 0 should become primary before timeout",
    replTest.timeoutMS,
);

replTest.awaitReplication();
jsTestLog("Checking that ops still replicate correctly");
let option = {writeConcern: {w: conns.length - 1, wtimeout: replTest.timeoutMS}};
// In PV0, this write can fail as a result of a bad spanning tree. If 2 was syncing from 4 prior
// to bridging, it will not change sync sources and receive the write in time. This was not a
// problem in 3.0 because the old version of mongobridge caused all the nodes to restart during
// partitioning, forcing the set to rebuild the spanning tree.
assert.commandWorked(primary.getDB("foo").bar.insert({x: 1}, option));

// 4 is connected to 3
conns[4].disconnect(conns[2]);
conns[4].reconnect(conns[3]);

assert.commandWorked(primary.getDB("foo").bar.insert({x: 1}, option));

replTest.stopSet();
