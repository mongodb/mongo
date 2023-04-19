/**
 * Tests that dbHash does not throw SnapshotUnavailable when running earlier than the latest DDL
 * operation for a collection in the database.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
"use strict";

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const db = primary.getDB("test");

const createTS = assert.commandWorked(db.createCollection(jsTestName())).operationTime;
jsTestLog("Create timestamp: " + tojson(createTS));

// Insert some data. Save the timestamp of the last insert.
let insertTS;
for (let i = 0; i < 10; i++) {
    insertTS = assert
                   .commandWorked(db.runCommand(
                       {insert: jsTestName(), documents: [{x: i}], writeConcern: {w: "majority"}}))
                   .operationTime;
}
jsTestLog("Last insert timestamp: " + tojson(insertTS));

// Perform a rename to bump the minimum visible snapshot timestamp on the collection.
const renameTS = assert.commandWorked(db[jsTestName()].renameCollection("renamed")).operationTime;
jsTestLog("Rename timestamp: " + tojson(renameTS));

// dbHash at all timestamps should work.
let res = assert.commandWorked(db.runCommand({
    dbHash: 1,
    $_internalReadAtClusterTime: createTS,
}));
assert(res.collections.hasOwnProperty(jsTestName()));

res = assert.commandWorked(db.runCommand({
    dbHash: 1,
    $_internalReadAtClusterTime: insertTS,
}));
assert(res.collections.hasOwnProperty(jsTestName()));

res = assert.commandWorked(db.runCommand({
    dbHash: 1,
    $_internalReadAtClusterTime: renameTS,
}));
assert(res.collections.hasOwnProperty("renamed"));

replTest.stopSet();
})();