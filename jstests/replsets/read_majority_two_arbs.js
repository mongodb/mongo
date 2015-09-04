// Test read committed and writeConcern: "majority" work properly with all other nodes being
// arbiters.
(function() {
"use strict";

// Set up a set and grab things for later.
var name = "read_majority_two_arbs";
var replTest = new ReplSetTest({name: name,
                                nodes: 3,
                                nodeOptions: {setParameter: "enableReplSnapshotThread=true"}});
var nodes = replTest.nodeList();
var conns = replTest.startSet();
replTest.initiate({"_id": name,
                   "members": [
                       {"_id": 0, "host": nodes[0]},
                       {"_id": 1, "host": nodes[1], arbiterOnly: true},
                       {"_id": 2, "host": nodes[2], arbiterOnly: true}]
                  });

var primary = replTest.getPrimary();
var db = primary.getDB(name);
var t = db[name];

function doDirtyRead() {
    var res = t.runCommand('find', {"readConcern": {"level": "local"}});
    assert.commandWorked(res);
    return new DBCommandCursor(db.getMongo(), res).toArray()[0].state;
}

function doCommittedRead() {
    var res = t.runCommand('find', {"readConcern": {"level": "majority"}});
    assert.commandWorked(res);
    return new DBCommandCursor(db.getMongo(), res).toArray()[0].state;
}

if (!db.serverStatus().storageEngine.supportsCommittedReads) {
    assert.neq(db.serverStatus().storageEngine.name, "wiredTiger");
    jsTest.log("skipping test since storage engine doesn't support committed reads");
    return;
}

assert.writeOK(t.save({_id: 1, state: 0}, {writeConcern: {w: "majority", wtimeout: 10*1000}}));
assert.eq(doDirtyRead(), 0);
assert.eq(doCommittedRead(), 0);
}());
