
var replTest = new ReplSetTest({ name: 'testSet', nodes: 5 });
var nodes = replTest.startSet({ oplogSize: "2" });
replTest.initiate();

jsTestLog("Replica set test initialized, reconfiguring to give one node higher priority");
var master = replTest.getMaster();
var config = master.getDB("local").system.replset.findOne();
config.version++;
config.members[0].priority = 2;

try {
    master.getDB("admin").runCommand({replSetReconfig : config});
}
catch(e) {
    print(e);
}

replTest.awaitSecondaryNodes();
// initial sync
master.getDB("foo").bar.insert({x:1});
replTest.awaitReplication();

jsTestLog("Bridging replica set");
master = replTest.bridge();

replTest.partition(0,4);
replTest.partition(1,2);
replTest.partition(2,3);
replTest.partition(3,1);

// 4 is connected to 2
replTest.partition(4,1);
replTest.partition(4,3);

jsTestLog("Checking that ops still replicate correctly");
master.getDB("foo").bar.insert({x:1});

var result = master.getDB("admin").runCommand({getLastError:1,w:5,wtimeout:30000});
assert.eq(null, result.err, tojson(result));

// 4 is connected to 3
replTest.partition(4,2);
replTest.unPartition(4,3);

master.getDB("foo").bar.insert({x:1});

result = master.getDB("admin").runCommand({getLastError:1,w:5,wtimeout:30000});
assert.eq(null, result.err, tojson(result));

replTest.stopSet();
