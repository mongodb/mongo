var replTest = new ReplSetTest({name: 'sync2', nodes: 5, useBridge: true});
var nodes = replTest.nodeList();
var conns = replTest.startSet({oplogSize: "2"});
replTest.initiate({
    "_id": "sync2",
    "members": [
        {"_id": 0, host: nodes[0], priority: 2},
        {"_id": 1, host: nodes[1]},
        {"_id": 2, host: nodes[2]},
        {"_id": 3, host: nodes[3]},
        {"_id": 4, host: nodes[4]}
    ]
});

var master = replTest.getPrimary();
jsTestLog("Replica set test initialized");

// initial sync
master.getDB("foo").bar.insert({x: 1});
replTest.awaitReplication();

conns[0].disconnect(conns[4]);
conns[1].disconnect(conns[2]);
conns[2].disconnect(conns[3]);
conns[3].disconnect(conns[1]);

// 4 is connected to 2
conns[4].disconnect(conns[1]);
conns[4].disconnect(conns[3]);

assert.soon(function() {
    master = replTest.getPrimary();
    return master === conns[0];
}, 60 * 1000, "node 0 did not become primary quickly enough");

replTest.awaitReplication();
jsTestLog("Checking that ops still replicate correctly");
var option = {writeConcern: {w: 5, wtimeout: 30000}};
// In PV0, this write can fail as a result of a bad spanning tree. If 2 was syncing from 4 prior to
// bridging, it will not change sync sources and receive the write in time. This was not a problem
// in 3.0 because the old version of mongobridge caused all the nodes to restart during
// partitioning, forcing the set to rebuild the spanning tree.
assert.writeOK(master.getDB("foo").bar.insert({x: 1}, option));

// 4 is connected to 3
conns[4].disconnect(conns[2]);
conns[4].reconnect(conns[3]);

option = {
    writeConcern: {w: 5, wtimeout: 30000}
};
assert.writeOK(master.getDB("foo").bar.insert({x: 1}, option));

replTest.stopSet();
