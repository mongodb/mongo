// Ensure removing a node without shutting it down does not cause a segfault (SERVER-13500)

// start a three node set
var name = "removeWithoutShutdown";
var replTest = new ReplSetTest({name: name, nodes: 3});
var nodes = replTest.startSet();
replTest.initiate();

// remove node 3 from node 2's config by editing the document in system.replset
nodes[1].getDB("local").system.replset.update({}, {$pop: {members: 1}});
// restart to load that version of the config
replTest.stop(1);
replTest.restart(1);
// assert that a member is gone from the config
assert.eq(2, nodes[1].getDB("local").system.replset.findOne().members.length,
          "incorrect number of nodes in config after reconfig");

// cause node 3 to sync from node 2
replTest.awaitSecondaryNodes();
assert.soon(function () {
    nodes[2].getDB("admin").runCommand({replSetSyncFrom: nodes[1].name});
    return replTest.status().members[2].syncingTo === nodes[1].name;
});
// this sleep is to wait for the actual connection and the problems it would cause
// sadly the above assert.soon does not suffice
sleep(2000);

// ensure node 2 is still up and happy (pre SERVER-13500 it would fall over and be sad)
assert.soon(function () {
    return nodes[1].getDB("local").system.replset.findOne().members.length == 2;
});

replTest.stopSet();
