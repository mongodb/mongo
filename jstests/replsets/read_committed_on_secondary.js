/**
 * @tags: [requires_journaling]
 *
 * Test basic read committed functionality on a secondary:
 *  - Updates should not be visible until they are in the blessed snapshot.
 *  - Updates should be visible once they are in the blessed snapshot.
 */

load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

(function() {
"use strict";

// Set up a set and grab things for later.
var name = "read_committed_on_secondary";
var replTest = new ReplSetTest({name: name,
                                nodes: 3,
                                nodeOptions: {enableMajorityReadConcern: ''}});

if (!startSetIfSupportsReadMajority(replTest)) {
    jsTest.log("skipping test since storage engine doesn't support committed reads");
    return;
}

var nodes = replTest.nodeList();
replTest.initiate({"_id": name,
                   "members": [
                       { "_id": 0, "host": nodes[0] },
                       { "_id": 1, "host": nodes[1], priority: 0 },
                       { "_id": 2, "host": nodes[2], arbiterOnly: true}]
                  });

// Get connections and collection.
var primary = replTest.getPrimary();
var secondary = replTest.liveNodes.slaves[0];
var secondaryId = replTest.getNodeId(secondary);

var dbPrimary = primary.getDB(name);
var collPrimary = dbPrimary[name];

var dbSecondary = secondary.getDB(name);
var collSecondary = dbSecondary[name];

function saveDoc(state) {
    var res = dbPrimary.runCommandWithMetadata(
        'update',
        {
            update: name,
            writeConcern: {w: 2, wtimeout: 60*1000},
            updates: [{q: {_id: 1}, u: {_id: 1, state: state}, upsert: true}],
        },
        {"$replData": 1});
    assert.commandWorked(res.commandReply);
    assert.eq(res.commandReply.writeErrors, undefined);
    return res.metadata.$replData.lastOpVisible;
}

function doDirtyRead(lastOp) {
    var res = collSecondary.runCommand('find', {"readConcern": {"level": "local",
                                                                "afterOpTime": lastOp}});
    assert.commandWorked(res);
    return new DBCommandCursor(secondary, res).toArray()[0].state;
}

function doCommittedRead(lastOp) {
    var res = collSecondary.runCommand('find', {"readConcern": {"level": "majority",
                                                                "afterOpTime": lastOp}});
    assert.commandWorked(res);
    return new DBCommandCursor(secondary, res).toArray()[0].state;
}

// Do a write, wait for it to replicate, and ensure it is visible.
var op0 = saveDoc(0);
assert.eq(doDirtyRead(op0), 0);
assert.eq(doCommittedRead(op0), 0);

// Disable snapshotting on the secondary.
secondary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'alwaysOn'});

// Do a write and ensure it is only visible to dirty reads
var op1 = saveDoc(1);
assert.eq(doDirtyRead(op1), 1);
assert.eq(doCommittedRead(op0), 0);

// Try the committed read again after sleeping to ensure it doesn't only work for queries
// immediately after the write.
sleep(1000);
assert.eq(doCommittedRead(op0), 0);

// Reenable snapshotting on the secondary and ensure that committed reads are able to see the new
// state.
secondary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'off'});
assert.eq(doDirtyRead(op1), 1);
assert.eq(doCommittedRead(op1), 1);
}());
