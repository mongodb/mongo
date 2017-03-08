load("jstests/replsets/rslib.js");

var name = 'slavedelay3';
var replTest = new ReplSetTest({name: name, nodes: 3, useBridge: true});
var nodes = replTest.startSet();
var config = replTest.getReplSetConfig();
// ensure member 0 is primary
config.members[0].priority = 2;
config.members[1].priority = 0;
config.members[1].slaveDelay = 5;
config.members[2].priority = 0;

replTest.initiate(config);
var master = replTest.getPrimary().getDB(name);

var slaveConns = replTest.liveNodes.slaves;
var slave = [];
for (var i in slaveConns) {
    var d = slaveConns[i].getDB(name);
    d.getMongo().setSlaveOk();
    slave.push(d);
}

waitForAllMembers(master);

nodes[0].disconnect(nodes[2]);

master.foo.insert({x: 1});

syncFrom(nodes[1], nodes[0], replTest);

// make sure the record still appears in the remote slave
assert.soon(function() {
    return slave[1].foo.findOne() != null;
});

replTest.stopSet();
