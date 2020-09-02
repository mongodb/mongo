// Tests the output of db.printSecondaryReplicationInfo() for unreachable secondaries.

(function() {
"use strict";
const name = "printSecondaryReplicationInfo";
const replSet = new ReplSetTest({name: name, nodes: 2});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const primary = replSet.getPrimary();
primary.getDB('test').foo.insert({a: 1});
replSet.awaitReplication();

const secondary = replSet.getSecondary();
replSet.stop(replSet.getNodeId(secondary));
replSet.waitForState(secondary, ReplSetTest.State.DOWN);

const joinShell =
    startParallelShell("db.getSiblingDB('admin').printSecondaryReplicationInfo();", primary.port);
joinShell();
assert(
    rawMongoProgramOutput().match("no replication info, yet.  State: \\(not reachable/healthy\\)"));

replSet.stopSet();
})();
