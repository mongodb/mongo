/**
 * Tests that a secondary can successfully apply an applyOps command that was created with an inner
 * "ts" field specified. If the primary receives such a command, it should apply it in non-atomic
 * mode since that is an over-specified applyOps oplog entry.
 *
 * This is a regression test for SERVER-44938.
 */

(function() {
"use strict";

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const collNss = primaryColl.getFullName();

jsTestLog("Do an insert");
const time =
    assert.commandWorked(primaryColl.runCommand('insert', {documents: [{_id: 0}]})).operationTime;
jsTestLog("Inserted with time " + tojson(time));
assert.commandWorked(primaryColl.insert({_id: 1}));

rst.awaitReplication();

const doc = {
    op: "i",
    ns: collNss,
    o: {_id: 3},
    ts: time,
    t: NumberLong(2)
};

jsTestLog("Run an applyOps command");

// Run an applyOps command with an inner "ts" field.
assert.commandWorked(primaryDB.runCommand({
    applyOps: [
        doc,
    ]
}));

rst.awaitReplication();

// Make sure that the secondary succesfully applies the applyOps oplog entry despite the command
// specifying an invalid inner "ts" field.
assert.sameMembers(primaryColl.find({_id: 3}).toArray(), [{_id: 3}]);
assert.sameMembers(secondary.getDB(dbName)[collName].find({_id: 3}).toArray(), [{_id: 3}]);

rst.stopSet();
})();
