var replTest = new ReplSetTest({name: 'sync2', nodes: 5});
var nodes = replTest.nodeList();
replTest.startSet({oplogSize: "2"});
replTest.initiate({"_id": "sync2",
                   "members": [
                       {"_id": 0, host: nodes[0], priority: 2},
                       {"_id": 1, host: nodes[1]},
                       {"_id": 2, host: nodes[2]},
                       {"_id": 3, host: nodes[3]},
                       {"_id": 4, host: nodes[4]}]
                 });

var master = replTest.getMaster();
jsTestLog("Replica set test initialized");

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
var option = { writeConcern: { w: 5, wtimeout: 60000 }};
assert.writeOK(master.getDB("foo").bar.insert({ x: 1 }, option));

// 4 is connected to 3
replTest.partition(4,2);
replTest.unPartition(4,3);

option = { writeConcern: { w: 5, wtimeout: 60000 }};
assert.writeOK(master.getDB("foo").bar.insert({ x: 1 }, option));

replTest.stopSet();
