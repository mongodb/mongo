// Tests that change streams on a collection can be resumed during and after the given collection is
// moved.

// @tags: [
//   requires_majority_read_concern,
//   uses_atclustertime,
//   requires_fcv_72,
//   featureFlagReshardingImprovements,
//   featureFlagMoveCollection,
//   featureFlagTrackUnshardedCollectionsUponCreation,
//   multiversion_incompatible,
// ]

import {ChangeStreamTest} from "jstests/libs/change_stream_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

// Use a higher frequency for periodic noops to speed up the test.
const reshardingTest = new ReshardingTest(
    {numDonors: 1, numRecipients: 1, periodicNoopIntervalSecs: 1, writePeriodicNoops: true});
reshardingTest.setup();

const kDbName = "reshardingDb";
const collName = "coll";
const ns = kDbName + "." + collName;

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection =
    reshardingTest.createUnshardedCollection({ns: ns, primaryShardName: donorShardNames[0]});

const mongos = sourceCollection.getMongo();
const reshardingDb = mongos.getDB(kDbName);

const cst = new ChangeStreamTest(reshardingDb);

// Open a change streams cursor on the collection that will be resharded.
let changeStreamsCursor =
    cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: collName});
assert.eq([], changeStreamsCursor.firstBatch, "Expected cursor not to have changes, but it did");

// We want to confirm that change streams can see events before, during, and after the resharding
// operation. Note in particular that this test confirms that a regular user change stream does
// NOT see internal resharding events such as reshardBegin.
const expectedChanges = [
    {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, oldKey: 0},
        ns: {db: kDbName, coll: collName},
        operationType: "insert",
    },
    {
        documentKey: {_id: 1},
        fullDocument: {_id: 1, oldKey: 1},
        ns: {db: kDbName, coll: collName},
        operationType: "insert",
    },
    {
        documentKey: {_id: 2},
        fullDocument: {_id: 2, oldKey: 2},
        ns: {db: kDbName, coll: collName},
        operationType: "insert",
    },
    {
        documentKey: {_id: 3},
        fullDocument: {_id: 3, newKey: 3, oldKey: 3},
        ns: {db: kDbName, coll: collName},
        operationType: "insert",
    },
    {
        documentKey: {_id: 4},
        fullDocument: {_id: 4, newKey: 4, oldKey: 4},
        ns: {db: kDbName, coll: collName},
        operationType: "insert",
    }
];
const preReshardCollectionChange = expectedChanges[0];
const midReshardCollectionChanges = expectedChanges.slice(1, 3);
const postReshardCollectionChanges = expectedChanges.slice(3);

// Verify that the cursor sees changes before the collection is resharded.
assert.commandWorked(sourceCollection.insert({_id: 0, oldKey: 0}));
const preReshardCollectionResumeToken =
    cst.assertNextChangesEqual(
           {cursor: changeStreamsCursor, expectedChanges: [preReshardCollectionChange]})[0]
        ._id;

const recipientShardNames = reshardingTest.recipientShardNames;
let midReshardCollectionResumeToken;
let changeStreamsCursor2;
reshardingTest.withMoveCollectionInBackground({toShard: recipientShardNames[0]}, () => {
    // Wait until participants are aware of the resharding operation.
    reshardingTest.awaitCloneTimestampChosen();

    // Open another change streams cursor while the collection is being resharded.
    changeStreamsCursor2 =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: collName});

    assert.commandWorked(sourceCollection.insert({_id: 1, oldKey: 1}));
    assert.commandWorked(sourceCollection.insert({_id: 2, oldKey: 2}));

    // Assert that both the cursors see the two new inserts.
    cst.assertNextChangesEqual(
        {cursor: changeStreamsCursor, expectedChanges: midReshardCollectionChanges});
    cst.assertNextChangesEqual(
        {cursor: changeStreamsCursor2, expectedChanges: midReshardCollectionChanges});

    // Check that we can resume from the token returned before resharding began.
    let resumedCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: preReshardCollectionResumeToken}}],
        collection: collName
    });
    midReshardCollectionResumeToken =
        cst.assertNextChangesEqual(
               {cursor: resumedCursor, expectedChanges: midReshardCollectionChanges})[1]
            ._id;
});

assert.commandWorked(sourceCollection.insert({_id: 3, newKey: 3, oldKey: 3}));

// Assert that both the cursor opened before resharding started and the one opened during
// resharding see the insert after resharding has finished.
cst.assertNextChangesEqual(
    {cursor: changeStreamsCursor, expectedChanges: [postReshardCollectionChanges[0]]});
cst.assertNextChangesEqual(
    {cursor: changeStreamsCursor2, expectedChanges: [postReshardCollectionChanges[0]]});

// Check that we can resume from both the token returned before resharding began and the token
// returned during resharding.
let resumedCursorFromPreOperation = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: preReshardCollectionResumeToken}}],
    collection: collName
});
let midAndPostReshardCollectionChanges =
    midReshardCollectionChanges.concat(postReshardCollectionChanges);

let resumedCursorFromMidOperation = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: midReshardCollectionResumeToken}}],
    collection: collName
});

assert.commandWorked(sourceCollection.insert({_id: 4, newKey: 4, oldKey: 4}));

cst.assertNextChangesEqual(
    {cursor: resumedCursorFromPreOperation, expectedChanges: midAndPostReshardCollectionChanges});
cst.assertNextChangesEqual(
    {cursor: resumedCursorFromMidOperation, expectedChanges: postReshardCollectionChanges});

reshardingTest.teardown();
