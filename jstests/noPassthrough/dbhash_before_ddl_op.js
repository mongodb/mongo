/**
 * Tests that dbHash does not throw SnapshotUnavailable when running earlier than the latest DDL
 * operation for a collection in the database. When the point-in-time catalog lookups feature flag
 * is disabled, SnapshotUnavailable is still thrown.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");

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

if (FeatureFlagUtil.isEnabled(db, "PointInTimeCatalogLookups")) {
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
} else {
    // dbHash at the 'createTS' should throw SnapshotUnavailable due to the rename.
    assert.commandFailedWithCode(db.runCommand({
        dbHash: 1,
        $_internalReadAtClusterTime: createTS,
    }),
                                 ErrorCodes.SnapshotUnavailable);

    // dbHash at the 'insertTS' should throw SnapshotUnavailable due to the rename.
    assert.commandFailedWithCode(db.runCommand({
        dbHash: 1,
        $_internalReadAtClusterTime: insertTS,
    }),
                                 ErrorCodes.SnapshotUnavailable);

    // dbHash at 'renameTS' should work.
    let res = assert.commandWorked(db.runCommand({
        dbHash: 1,
        $_internalReadAtClusterTime: renameTS,
    }));
    assert(res.collections.hasOwnProperty("renamed"));
}

replTest.stopSet();
})();