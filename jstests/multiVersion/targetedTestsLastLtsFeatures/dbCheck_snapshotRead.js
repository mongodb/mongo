/**
 * Ensure that a 6.0 version replicating a dbCheck oplog entry with the removed snapshotRead:false
 * option does not crash when a 'latest' version receives the entry.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/multiVersion/libs/multi_rs.js');

const nodes = {
    // We want the 6.0 node to be the primary.
    n1: {binVersion: "6.0", rsConfig: {priority: 1}},
    n2: {binVersion: "latest", rsConfig: {priority: 0}},
};

const rst = new ReplSetTest({nodes: nodes});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = jsTestName();

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const coll = primaryDB[collName];

assert.commandWorked(coll.insert({a: 1}));

// The 6.0 node will replicate the dbCheck oplog entry with the 'snapshotRead:false' option. This is
// not supported in recent versions and should be ignored, but not cause the node to crash.
assert.commandWorked(primaryDB.runCommand({"dbCheck": 1, snapshotRead: false}));

rst.awaitReplication();

function dbCheckCompleted(db) {
    return db.currentOp().inprog.filter(x => x["desc"] == "dbCheck")[0] === undefined;
}

function forEachNode(f) {
    f(rst.getPrimary());
    f(rst.getSecondary());
}

function awaitDbCheckCompletion(db) {
    assert.soon(() => dbCheckCompleted(db), "dbCheck timed out");
    rst.awaitSecondaryNodes();
    rst.awaitReplication();

    forEachNode(function(node) {
        const healthlog = node.getDB('local').system.healthlog;
        assert.soon(function() {
            return (healthlog.find({"operation": "dbCheckStop"}).itcount() == 1);
        }, "dbCheck command didn't complete");
    });
}

awaitDbCheckCompletion(primaryDB);

{
    // The 6.0 primary should not report any errors.
    const healthlog = primary.getDB('local').system.healthlog;
    assert.eq(0, healthlog.find({severity: "error"}).itcount());
    assert.eq(0, healthlog.find({severity: "warning"}).itcount());
}

{
    // The latest secondary should log an error in the health log.
    const secondary = rst.getSecondary();
    const healthlog = secondary.getDB('local').system.healthlog;
    assert.eq(1, healthlog.find({severity: "error"}).itcount());
    assert.eq(0, healthlog.find({severity: "warning"}).itcount());
    const errorEntry = healthlog.findOne({severity: "error"});
    assert(errorEntry.hasOwnProperty('data'), tojson(errorEntry));
    assert.eq(false, errorEntry.data.success, tojson(errorEntry));
    assert(errorEntry.data.error.startsWith("Location6769502"), tojson(errorEntry));
}

rst.stopSet();
})();
