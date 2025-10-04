// ensure removing a chained node does not break reporting of replication progress (SERVER-15849)

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {syncFrom} from "jstests/replsets/rslib.js";

let numNodes = 5;
let host = getHostName();
let name = "chaining_removal";

let replTest = new ReplSetTest({name: name, nodes: numNodes});
let nodes = replTest.startSet();
let port = replTest.ports;
replTest.initiate({
    _id: name,
    members: [
        {_id: 0, host: nodes[0].host, priority: 3},
        {_id: 1, host: nodes[1].host, priority: 0},
        {_id: 2, host: nodes[2].host, priority: 0},
        {_id: 3, host: nodes[3].host, priority: 0},
        {_id: 4, host: nodes[4].host, priority: 0},
    ],
});
replTest.awaitNodesAgreeOnPrimary(replTest.timeoutMS, nodes, nodes[0]);
let primary = replTest.getPrimary();
// The default WC is majority and stopServerReplication could prevent satisfying any majority
// writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

replTest.awaitReplication();

// When setting up chaining on slow machines, we do not want slow writes or delayed heartbeats
// to cause our nodes to invalidate the sync source provided in the 'replSetSyncFrom' command.
// To achieve this, we disable the server parameter 'maxSyncSourceLagSecs' (see
// repl_settings_init.cpp and TopologyCoordinatorImpl::Options) in
// TopologyCoordinatorImpl::shouldChangeSyncSource().
assert.commandWorked(
    nodes[1].getDB("admin").runCommand({configureFailPoint: "disableMaxSyncSourceLagSecs", mode: "alwaysOn"}),
);
assert.commandWorked(
    nodes[4].getDB("admin").runCommand({configureFailPoint: "disableMaxSyncSourceLagSecs", mode: "alwaysOn"}),
);

// Force node 1 to sync directly from node 0.
syncFrom(nodes[1], nodes[0], replTest);
// Force node 4 to sync through node 1.
syncFrom(nodes[4], nodes[1], replTest);

// write that should reach all nodes
let timeout = ReplSetTest.kDefaultTimeoutMS;
let options = {writeConcern: {w: numNodes, wtimeout: timeout}};
assert.commandWorked(primary.getDB(name).foo.insert({x: 1}, options));

// Re-enable 'maxSyncSourceLagSecs' checking on sync source.
assert.commandWorked(
    nodes[1].getDB("admin").runCommand({configureFailPoint: "disableMaxSyncSourceLagSecs", mode: "off"}),
);
assert.commandWorked(
    nodes[4].getDB("admin").runCommand({configureFailPoint: "disableMaxSyncSourceLagSecs", mode: "off"}),
);

let config = primary.getDB("local").system.replset.findOne();
config.members.pop();
config.version++;
// remove node 4
replTest.stop(4);
try {
    primary.adminCommand({replSetReconfig: config});
} catch (e) {
    print("error: " + e);
}

// ensure writing to all four nodes still works
primary = replTest.getPrimary();
const liveSecondaries = [nodes[1], nodes[2], nodes[3]];
replTest.awaitReplication(null, null, liveSecondaries);
options.writeConcern.w = 4;
assert.commandWorked(primary.getDB(name).foo.insert({x: 2}, options));

replTest.stopSet();
