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
  
var replTest = new ReplSetTest( {name: name, nodes: 3} );

var nodes = replTest.startSet();

/* set slaveDelay to 30 seconds */
var config = replTest.getReplSetConfig();
config.members[2].priority = 0;
  
replTest.initiate(config);

var master = replTest.getMaster().getDB("test");
var server0 = master;
var server1 = replTest.liveNodes.slaves[0];

print("Initial sync");
for (var i=0;i<100;i++) {
    master.foo.insert({x:i});
}
replTest.awaitReplication();


print("stop #1");
replTest.stop(1);


print("add some data");
for (var i=0;i<1000;i++) {
    master.bar.insert({x:i});
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
master = replTest.getMaster().getDB("test");
for (var i=0;i<1000;i++) {
    master.bar.insert({x:i});
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
