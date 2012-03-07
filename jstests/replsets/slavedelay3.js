load("jstests/replsets/rslib.js");

var name = 'slavedelay3';
var replTest = new ReplSetTest({ name: name, nodes: 3 });
var nodes = replTest.startSet();
var config = replTest.getReplSetConfig();
// ensure member 0 is primary
config.members[0].priority = 2;
config.members[1].priority = 0;
config.members[1].slaveDelay = 5;

replTest.initiate(config);
replTest.awaitReplication();
replTest.bridge();


var master = replTest.getMaster().getDB(name);
 var slaveConns = replTest.liveNodes.slaves;
 var slave = [];
 for (var i in slaveConns) {
     var d = slaveConns[i].getDB(name);
     d.getMongo().setSlaveOk();
     slave.push(d);
 }

waitForAllMembers(master);



replTest.partition(0,2);
replTest.awaitReplication();

master.foo.insert({x:1});

// make sure the record never appears in the remote slave
for (var i=0; i<20; i++) {
    print ("testing for data!");
    assert.eq(slave[1].foo.findOne(), null);
    sleep(3000);
    stat = slave[1]._adminCommand("replSetGetStatus");
    printjson(stat);
}



replTest.stopSet();