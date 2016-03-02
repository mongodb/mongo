/**
 * Test basic read committed functionality on a secondary:
 *  - Updates should not be visible until they are in the blessed snapshot.
 *  - Updates should be visible once they are in the blessed snapshot.
 */

load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

(function() {
    "use strict";

    function printStatus() {
        var primaryStatus;
        replTest.nodes.forEach((n) => {
            var status = n.getDB("admin").runCommand("replSetGetStatus");
            var self = status.members.filter((m) => m.self)[0];
            var msg = self.name + "\n";
            msg += tojson(status.optimes) + "\n";
            if (self.state == 1) {  // Primary status.
                // List other members status from the primaries perspective
                msg += tojson(status.members.filter((m) => !m.self)) + "\n";
                msg += tojson(status.slaveInfo) + "\n";
            }
            jsTest.log(msg);
        });
    }

    function log(arg) {
        jsTest.log(tojson(arg));
    }
    // Set up a set and grab things for later.
    var name = "read_committed_on_secondary";
    var replTest =
        new ReplSetTest({name: name, nodes: 3, nodeOptions: {enableMajorityReadConcern: ''}});

    if (!startSetIfSupportsReadMajority(replTest)) {
        log("skipping test since storage engine doesn't support committed reads");
        return;
    }

    var nodes = replTest.nodeList();
    var config = {
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1], priority: 0},
            {"_id": 2, "host": nodes[2], arbiterOnly: true}
        ]
    };
    updateConfigIfNotDurable(config);
    replTest.initiate(config);

    // Get connections and collection.
    var primary = replTest.getPrimary();
    var secondary = replTest.liveNodes.slaves[0];
    var secondaryId = replTest.getNodeId(secondary);

    var dbPrimary = primary.getDB(name);
    var collPrimary = dbPrimary[name];

    var dbSecondary = secondary.getDB(name);
    var collSecondary = dbSecondary[name];

    function saveDoc(state) {
        log("saving doc.");
        var res = dbPrimary.runCommandWithMetadata(
            'update',
            {
              update: name,
              writeConcern: {w: 2, wtimeout: 60 * 1000},
              updates: [{q: {_id: 1}, u: {_id: 1, state: state}, upsert: true}],
            },
            {"$replData": 1});
        assert.commandWorked(res.commandReply);
        assert.eq(res.commandReply.writeErrors, undefined);
        log("done saving doc: optime " + tojson(res.metadata.$replData.lastOpVisible));
        return res.metadata.$replData.lastOpVisible;
    }

    function doDirtyRead(lastOp) {
        log("doing dirty read for lastOp:" + tojson(lastOp));
        var res = collSecondary.runCommand(
            'find', {"readConcern": {"level": "local", "afterOpTime": lastOp}, "maxTimeMS": 3000});
        assert.commandWorked(res);
        log("done doing dirty read.");
        return new DBCommandCursor(secondary, res).toArray()[0].state;
    }

    function doCommittedRead(lastOp) {
        log("doing committed read for optime: " + tojson(lastOp));
        var res = collSecondary.runCommand(
            'find',
            {"readConcern": {"level": "majority", "afterOpTime": lastOp}, "maxTimeMS": 3000});
        assert.commandWorked(res);
        log("done doing committed read.");
        return new DBCommandCursor(secondary, res).toArray()[0].state;
    }

    // Do a write, wait for it to replicate, and ensure it is visible.
    var op0 = saveDoc(0);
    assert.eq(doDirtyRead(op0), 0);

    printStatus();
    assert.eq(doCommittedRead(op0), 0);

    // Disable snapshotting on the secondary.
    secondary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'alwaysOn'});

    // Do a write and ensure it is only visible to dirty reads
    var op1 = saveDoc(1);
    assert.eq(doDirtyRead(op1), 1);
    assert.eq(doCommittedRead(op0), 0);

    // Try the committed read again after sleeping to ensure it doesn't only work for queries
    // immediately after the write.
    log("sleeping");
    sleep(1000);
    assert.eq(doCommittedRead(op0), 0);

    // Reenable snapshotting on the secondary and ensure that committed reads are able to see the
    // new
    // state.
    log("turning off failpoint");
    secondary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'off'});
    assert.eq(doDirtyRead(op1), 1);
    log(replTest.status());
    replTest.awaitReplication();
    log(replTest.status());
    assert.eq(doCommittedRead(op1), 1);
    log("test success!");
    replTest.stopSet();
}());
