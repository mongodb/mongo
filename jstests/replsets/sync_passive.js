/**
 * Test syncing from non-primaries.
 *
 * Start a set.
 * Inital sync.
 * Kill member 1.
 * Add some data.
 * Kill member 0.
 * Restart member 1.
 * Check that it syncs.
 * Add some data.
 * Kill member 1.
 * Restart member 0.
 * Check that it syncs.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {reconnect} from "jstests/replsets/rslib.js";

let name = "sync_passive";
let host = getHostName();

let replTest = new ReplSetTest({name: name, nodes: 3});

let nodes = replTest.startSet();

// make sure node 0 becomes primary initially and that node 2 never will
let config = replTest.getReplSetConfig();
config.members[0].priority = 2;
config.members[2].priority = 0;

replTest.initiate(config, null, {initiateWithDefaultElectionTimeout: true});
replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);

let primary = replTest.getPrimary().getDB("test");
let server0 = primary;
let server1 = replTest.getSecondary();

print("Initial sync");
for (var i = 0; i < 100; i++) {
    primary.foo.insert({x: i});
}
replTest.awaitReplication();

print("stop #1");
replTest.stop(1);

print("add some data");
for (var i = 0; i < 1000; i++) {
    primary.bar.insert({x: i});
}
const liveSecondaries = [replTest.nodes[2]];
replTest.awaitReplication(null, null, liveSecondaries);

print("stop #0");
replTest.stop(0);

print("restart #1");
replTest.restart(1);

print("check sync");
replTest.awaitReplication(null, null, liveSecondaries);

print("add data");
reconnect(server1);
primary = replTest.getPrimary().getDB("test");
for (var i = 0; i < 1000; i++) {
    primary.bar.insert({x: i});
}
replTest.awaitReplication(null, null, liveSecondaries);

print("kill #1");
replTest.stop(1);

print("restart #0");
replTest.restart(0);
reconnect(server0);

print("wait for sync");
replTest.awaitReplication(null, null, liveSecondaries);

print("bring #1 back up, make sure everything's okay");
replTest.restart(1);

replTest.stopSet();
