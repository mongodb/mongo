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

load("jstests/replsets/rslib.js");

var name = "sync_passive";
var host = getHostName();

var replTest = new ReplSetTest({name: name, nodes: 3});

var nodes = replTest.startSet();

// make sure node 0 becomes primary initially and that node 2 never will
var config = replTest.getReplSetConfig();
config.members[0].priority = 2;
config.members[2].priority = 0;

replTest.initiate(config);
replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);

var master = replTest.getPrimary().getDB("test");
var server0 = master;
var server1 = replTest.liveNodes.slaves[0];

print("Initial sync");
for (var i = 0; i < 100; i++) {
    master.foo.insert({x: i});
}
replTest.awaitReplication();

print("stop #1");
replTest.stop(1);

print("add some data");
for (var i = 0; i < 1000; i++) {
    master.bar.insert({x: i});
}
replTest.awaitReplication();

print("stop #0");
replTest.stop(0);

print("restart #1");
replTest.restart(1);

print("check sync");
replTest.awaitReplication();

print("add data");
reconnect(server1);
master = replTest.getPrimary().getDB("test");
for (var i = 0; i < 1000; i++) {
    master.bar.insert({x: i});
}
replTest.awaitReplication();

print("kill #1");
replTest.stop(1);

print("restart #0");
replTest.restart(0);
reconnect(server0);

print("wait for sync");
replTest.awaitReplication();

print("bring #1 back up, make sure everything's okay");
replTest.restart(1);
