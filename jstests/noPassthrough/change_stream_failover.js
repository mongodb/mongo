// Test resuming a change stream on a node other than the one it was started on. Accomplishes this
// by triggering a stepdown.
// This test uses the WiredTiger storage engine, which does not support running without journaling.
// @tags: [
//   requires_majority_read_concern,
//   requires_replication,
// ]
(function() {
"use strict";
load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

for (let key of Object.keys(ChangeStreamWatchMode)) {
    const watchMode = ChangeStreamWatchMode[key];
    jsTestLog("Running test for mode " + watchMode);

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB("test");
    const coll = assertDropAndRecreateCollection(primaryDB, "change_stream_failover");

    // Be sure we'll only read from the primary.
    primary.setReadPref("primary");

    // Open a changeStream on the primary.
    const cst = new ChangeStreamTest(ChangeStreamTest.getDBForChangeStream(watchMode, primaryDB));

    let changeStream = cst.getChangeStream({watchMode: watchMode, coll: coll});

    // Be sure we can read from the change stream. Use {w: "majority"} so that we're still
    // guaranteed to be able to read after the failover.
    assert.commandWorked(coll.insert({_id: 0}, {writeConcern: {w: "majority"}}));
    assert.commandWorked(coll.insert({_id: 1}, {writeConcern: {w: "majority"}}));
    assert.commandWorked(coll.insert({_id: 2}, {writeConcern: {w: "majority"}}));

    const firstChange = cst.getOneChange(changeStream);
    assert.docEq(firstChange.fullDocument, {_id: 0});

    // Make the primary step down
    assert.commandWorked(primaryDB.adminCommand({replSetStepDown: 30}));

    // Now wait for another primary to be elected.
    const newPrimary = rst.getPrimary();
    // Be sure we got a different node that the previous primary.
    assert.neq(newPrimary.port, primary.port);

    cst.assertNextChangesEqual({
        cursor: changeStream,
        expectedChanges: [{
            documentKey: {_id: 1},
            fullDocument: {_id: 1},
            ns: {db: primaryDB.getName(), coll: coll.getName()},
            operationType: "insert",
        }]
    });

    // Now resume using the resume token from the first change (before the failover).
    const resumeCursor =
        cst.getChangeStream({watchMode: watchMode, coll: coll, resumeAfter: firstChange._id});

    // Be sure we can read the 2nd and 3rd changes.
    cst.assertNextChangesEqual({
        cursor: resumeCursor,
        expectedChanges: [
            {
                documentKey: {_id: 1},
                fullDocument: {_id: 1},
                ns: {db: primaryDB.getName(), coll: coll.getName()},
                operationType: "insert",
            },
            {
                documentKey: {_id: 2},
                fullDocument: {_id: 2},
                ns: {db: primaryDB.getName(), coll: coll.getName()},
                operationType: "insert",
            }
        ]
    });

    // Unfreeze the original primary so that it can stand for election again.
    assert.commandWorked(primaryDB.adminCommand({replSetFreeze: 0}));
}

rst.stopSet();
}());
