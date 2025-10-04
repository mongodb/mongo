// Test that a delayed secondary in a replica set still succeeds as a sync source.
// First, we disconnect the primary from the non-delayed secondary. Next, we issue
// a write to the primary and ensure this write propagates
// to the disconnected node via the delayed secondary.
//
// @tags: [
// ]
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {syncFrom, waitForAllMembers} from "jstests/replsets/rslib.js";

let replTest = new ReplSetTest({nodes: 3, useBridge: true});
let nodes = replTest.startSet();
let config = replTest.getReplSetConfig();
// ensure member 0 is primary
config.members[0].priority = 2;
config.members[1].priority = 0;
config.members[1].secondaryDelaySecs = 5;
config.members[2].priority = 0;

replTest.initiate(config);
let primary = replTest.getPrimary().getDB(jsTestName());
// The default WC is majority and stopServerReplication could prevent satisfying any majority
// writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

replTest.awaitReplication();

let secondaryConns = replTest.getSecondaries();
let secondaries = [];
for (let i in secondaryConns) {
    let d = secondaryConns[i].getDB(jsTestName());
    d.getMongo().setSecondaryOk();
    secondaries.push(d);
}

waitForAllMembers(primary);

nodes[0].disconnect(nodes[2]);

primary.foo.insert({x: 1});

syncFrom(nodes[1], nodes[0], replTest);

// make sure the record still appears in the remote secondary
assert.soon(function () {
    return secondaries[1].foo.findOne() != null;
});

replTest.stopSet();
