// Test that resume tokens from a replica set running the "last-stable" mongod can be used to resume
// a change stream after upgrading the replica set to the "latest" mongod, even when the change
// stream includes multi-statement transactions.
//
// @tags: [uses_change_streams, uses_transactions, requires_replication]
(function() {
"use strict";

load('jstests/multiVersion/libs/multi_rs.js');  // For upgradeSet.
load("jstests/replsets/rslib.js");              // For startSetIfSupportsReadMajority.

const rst = new ReplSetTest({
    nodes: 2,
    nopeOptions: {binVersion: "last-stable"},
});

if (!startSetIfSupportsReadMajority(rst)) {
    jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
    rst.stopSet();
    return;
}

rst.initiate();

const dbName = jsTestName();
const watchedCollName = "change_stream_watched";
const unwatchedCollName = "change_stream_unwatched";

rst.getPrimary().getDB(dbName).createCollection(watchedCollName);
rst.getPrimary().getDB(dbName).createCollection(unwatchedCollName);

// Calls next() on a change stream cursor 'n' times and returns an array with the results.
function getChangeStreamResults(cursor, n) {
    let results = [];
    for (let i = 0; i < n; ++i) {
        assert.soon(() => cursor.hasNext(), "Timed out waiting for change stream result " + i);
        results.push(cursor.next());
    }
    assert(!cursor.hasNext());  // The change stream should always have exactly 'n' results.
    return results;
}

// Compare expected changes with output from a change stream, failing an assertion if they do
// not match.
function compareChanges(expectedChanges, observedChanges) {
    assert.eq(expectedChanges.length, observedChanges.length);
    for (let i = 0; i < expectedChanges.length; ++i) {
        assert.eq(expectedChanges[i].operationType, observedChanges[i].operationType);
        if (expectedChanges[i].hasOwnProperty("fullDocument")) {
            assert.eq(expectedChanges[i].fullDocument, observedChanges[i].fullDocument);
        }
        if (expectedChanges[i].hasOwnProperty("updateDescription")) {
            assert.eq(expectedChanges[i].updateDescription, observedChanges[i].updateDescription);
        }
        if (expectedChanges[i].hasOwnProperty("documentKey")) {
            assert.eq(expectedChanges[i].documentKey, observedChanges[i].documentKey);
        }
    }
}

// Opens a $changeStream and then performs inserts, deletes, updates both within a transaction
// and outside the transaction. Leaves all collections empty when done.
function performDBOps(mongod) {
    const session = mongod.startSession();
    session.startTransaction();

    const watchedColl = session.getDatabase(dbName)[watchedCollName];
    assert.commandWorked(watchedColl.insert({_id: 1}));
    assert.commandWorked(watchedColl.updateOne({_id: 1}, {$set: {a: 1}}));
    assert.commandWorked(watchedColl.remove({_id: 1}));

    const unwatchedColl = session.getDatabase(dbName)[unwatchedCollName];
    assert.commandWorked(unwatchedColl.insert({_id: 1}));
    assert.commandWorked(unwatchedColl.remove({_id: 1}));

    const watchedCollNoTxn = mongod.getDB(dbName)[watchedCollName];
    assert.commandWorked(watchedCollNoTxn.insert({_id: 2}));
    assert.commandWorked(watchedCollNoTxn.remove({_id: 2}));

    session.commitTransaction();
}

// Resume a change stream from each of the resume tokens in the 'changeStreamDocs' array and
// verify that we always see the same set of changes.
function resumeChangeStreamFromEachToken(mongod, changeStreamDocs, expectedChanges) {
    changeStreamDocs.forEach(function(changeDoc, i) {
        const testDB = mongod.getDB(dbName);
        const resumedCursor = testDB[watchedCollName].watch([], {resumeAfter: changeDoc._id});

        // Resuming from document 'i' should return all the documents from 'i' + 1 to the end of
        // the array.
        const expectedChangesAfterResumeToken = expectedChanges.slice(i + 1);
        compareChanges(
            expectedChangesAfterResumeToken,
            getChangeStreamResults(resumedCursor, expectedChangesAfterResumeToken.length));
    });
}

const expectedChanges = [
    {operationType: "insert", fullDocument: {_id: 2}},
    {operationType: "delete", documentKey: {_id: 2}},
    {operationType: "insert", fullDocument: {_id: 1}},
    {operationType: "update", updateDescription: {updatedFields: {a: 1}, removedFields: []}},
    {operationType: "delete", documentKey: {_id: 1}},
];

// Create the original change stream, verify it gives us the changes we expect, and verify that
// we can correctly resume from any resume token.
const changeStreamCursor = rst.getPrimary().getDB(dbName)[watchedCollName].watch();
performDBOps(rst.getPrimary());
const changeStreamDocs = getChangeStreamResults(changeStreamCursor, expectedChanges.length);
compareChanges(expectedChanges, changeStreamDocs);
resumeChangeStreamFromEachToken(rst.getPrimary(), changeStreamDocs, expectedChanges);

// Upgrade the replica set (while leaving featureCompatibilityVersion as it is) and verify that
// we can correctly resume from any resume token.
rst.upgradeSet({binVersion: "latest"});
resumeChangeStreamFromEachToken(rst.getPrimary(), changeStreamDocs, expectedChanges);

// Upgrade the featureCompatibilityVersion and verify that we can correctly resume from any
// resume token.
assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(rst.getPrimary().getDB("admin"), latestFCV);
resumeChangeStreamFromEachToken(rst.getPrimary(), changeStreamDocs, expectedChanges);

rst.stopSet();
}());
