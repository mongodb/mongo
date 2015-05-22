load("jstests/replsets/rslib.js");

var name = 'slavedelay3';
var replTest = new ReplSetTest({ name: name, nodes: 3 });
var nodes = replTest.startSet();
var config = replTest.getReplSetConfig();
var host = getHostName();
// ensure member 0 is primary
config.members[0].priority = 2;
config.members[1].priority = 0;
config.members[1].slaveDelay = 5;
config.members[2].priority = 0;

replTest.initiate(config);
var master = replTest.getMaster().getDB(name);
replTest.awaitReplication();
replTest.bridge();

var slaveConns = replTest.liveNodes.slaves;
var slave = [];
for (var i in slaveConns) {
    var d = slaveConns[i].getDB(name);
    d.getMongo().setSlaveOk();
    slave.push(d);
}

waitForAllMembers(master);



replTest.awaitReplication();
replTest.partition(0,2);

master.foo.insert({x:1});

var primaryFromNode1 = host + ':' + replTest.bridges[1][0].port;
assert.commandWorked(nodes[1].getDB("admin").runCommand({"replSetSyncFrom": primaryFromNode1}));
var res;
assert.soon(function() {
    res = nodes[1].getDB("admin").runCommand({"replSetGetStatus": 1});
    return res.syncingTo === primaryFromNode1;
}, "node 4 failed to start chaining: "+ tojson(res));

// make sure the record still appears in the remote slave
assert.soon( function() { return slave[1].foo.findOne() != null; } );

replTest.stopSet();
