// Test basic read committed functionality.
(function() {
"use strict";

// Set up a set and grab things for later.
var name = "read_committed";
var replTest = new ReplSetTest({name: name,
                                nodes: 3,
                                nodeOptions: {setParameter: "enableReplSnapshotThread=true"}});
var nodes = replTest.nodeList();
var conns = replTest.startSet();
replTest.initiate({"_id": name,
                   "members": [
                       { "_id": 0, "host": nodes[0] },
                       { "_id": 1, "host": nodes[1] },
                       { "_id": 2, "host": nodes[2], arbiterOnly: true}]
                  });

// Get connections and collection.
var master = replTest.getMaster();
var slave = replTest.liveNodes.slaves[0];
var slaveId = replTest.getNodeId(slave);
var db = master.getDB(name);
var t = db[name];

if (!db.serverStatus().storageEngine.supportsCommittedReads) {
    assert.neq(db.serverStatus().storageEngine.name, "wiredTiger");
    jsTest.log("skipping test since storage engine doesn't support committed reads");
    return;
}

function doDirtyRead() {
    var res = t.runCommand('find', {$readMajorityTemporaryName: false});
    assert.commandWorked(res);
    return new DBCommandCursor(db.getMongo(), res).toArray()[0].state;
}

function doCommittedRead() {
    var res = t.runCommand('find', {$readMajorityTemporaryName: true});
    assert.commandWorked(res);
    return new DBCommandCursor(db.getMongo(), res).toArray()[0].state;
}

// Do a write, wait for it to replicate, and ensure it is visible.
assert.writeOK(t.save({_id: 1, state: 0}, {writeConcern: {w: "majority", wtimeout: 60*1000}}));
assert.eq(doDirtyRead(), 0);
assert.eq(doCommittedRead(), 0);

replTest.stop(slaveId);

// Do a write and ensure it is only visible to dirty reads
assert.writeOK(t.save({_id: 1, state: 1}));
assert.eq(doDirtyRead(), 1);
assert.eq(doCommittedRead(), 0);

// Try the committed read again after sleeping to ensure it doesn't only work for queries
// immediately after the write.
sleep(1000);
assert.eq(doCommittedRead(), 0);

// Restart the node and ensure the committed view is updated.
replTest.restart(slaveId);
db.getLastError("majority", 60*1000);
assert.eq(doDirtyRead(), 1);
assert.eq(doCommittedRead(), 1);
}());
