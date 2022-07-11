// Tests of $changeStream notifications for metadata operations.
// Do not run in whole-cluster passthrough since this test assumes that the change stream will be
// invalidated by a database drop.
// @tags: [do_not_run_in_whole_cluster_passthrough]
(function() {
"use strict";

load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.
load('jstests/replsets/libs/two_phase_drops.js');  // For 'TwoPhaseDropCollectionTest'.
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
load("jstests/libs/fixture_helpers.js");           // For isSharded.

db = db.getSiblingDB(jsTestName());
let cst = new ChangeStreamTest(db);

// Test that it is possible to open a new change stream cursor on a collection that does not
// exist.
const collName = "test";
assertDropCollection(db, collName);

// Asserts that resuming a change stream with 'spec' and an explicit simple collation returns
// the results specified by 'expected'.
function assertResumeExpected({coll, spec, expected}) {
    const cursor = cst.startWatchingChanges({
        collection: coll,
        pipeline: [{$changeStream: spec}],
        aggregateOptions: {collation: {locale: "simple"}}
    });
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});
}

// Cursor creation succeeds, but there are no results. We do not expect to see a notification
// for collection creation.
let cursor = cst.startWatchingChanges(
    {collection: collName, pipeline: [{$changeStream: {}}, {$project: {operationType: 1}}]});

// We explicitly test getMore, to ensure that the getMore command for a non-existent collection
// does not return an error.
let change = cst.getNextBatch(cursor);
assert.neq(change.id, 0);
assert.eq(change.nextBatch.length, 0, tojson(change.nextBatch));

// Dropping the empty database should not generate any notification for the change stream, since
// the collection does not exist yet.
assert.commandWorked(db.dropDatabase());
change = cst.getNextBatch(cursor);
assert.neq(change.id, 0);
assert.eq(change.nextBatch.length, 0, tojson(change.nextBatch));

// After collection creation, we expect to see oplog entries for each subsequent operation.
let coll = assertCreateCollection(db, collName);
assert.commandWorked(coll.insert({_id: 0}));

// Determine the number of shards that the collection is distributed across.
const numShards = FixtureHelpers.numberOfShardsForCollection(coll);

change = cst.getOneChange(cursor);
assert.eq(change.operationType, "insert", tojson(change));

// Create oplog entries of type insert, update, delete, and drop.
assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(coll.update({_id: 1}, {$set: {a: 1}}));
assert.commandWorked(coll.remove({_id: 1}));
assertDropCollection(db, coll.getName());

// We should get oplog entries of type insert, update, delete, drop, and invalidate. The cursor
// should be closed.
let expectedChanges = [
    {operationType: "insert"},
    {operationType: "update"},
    {operationType: "delete"},
    {operationType: "drop"},
    {operationType: "invalidate"},
];
let changes = cst.assertNextChangesEqual(
    {cursor: cursor, expectedChanges: expectedChanges, expectInvalidate: true});
const resumeToken = changes[0]._id;
const resumeTokenDrop = changes[3]._id;
const resumeTokenInvalidate = changes[4]._id;

// Verify we can startAfter the invalidate. We should see one drop event for every other shard
// that the collection was present on, or nothing if the collection was not sharded. This test
// exercises the bug described in SERVER-41196.
const restartedStream = coll.watch([], {startAfter: resumeTokenInvalidate});
for (let i = 0; i < numShards - 1; ++i) {
    assert.soon(() => restartedStream.hasNext());
    const nextEvent = restartedStream.next();
    assert.eq(nextEvent.operationType, "drop", () => tojson(nextEvent));
}
assert(!restartedStream.hasNext(), () => tojson(restartedStream.next()));

// Verify that we can resume a stream after a collection drop without an explicit collation.
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
    cursor: {}
}));

// Recreate the collection.
coll = assertCreateCollection(db, collName);
assert.commandWorked(coll.insert({_id: "after recreate"}));

// Test resuming the change stream from the collection drop using 'resumeAfter'. If running in a
// sharded passthrough suite, resuming from the drop will first return the drop from the other
// shard before returning an invalidate.
cursor = cst.startWatchingChanges({
    collection: coll,
    pipeline: [{$changeStream: {resumeAfter: resumeTokenDrop}}],
    aggregateOptions: {collation: {locale: "simple"}, cursor: {batchSize: 0}}
});
cst.consumeDropUpTo({
    cursor: cursor,
    dropType: "drop",
    expectedNext: {operationType: "invalidate"},
    expectInvalidate: true
});

// Test resuming the change stream from the invalidate after the drop using 'resumeAfter'.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$changeStream: {resumeAfter: resumeTokenInvalidate}}],
    cursor: {},
    collation: {locale: "simple"},
}),
                             ErrorCodes.InvalidResumeToken);

// Even after the 'invalidate' event has been filtered out, the cursor should hold the resume token
// of the 'invalidate' event.
const resumeStream =
    coll.watch([{$match: {operationType: "DummyOperationType"}}], {resumeAfter: resumeToken});
assert.soon(() => {
    assert(!resumeStream.hasNext());
    return resumeStream.isExhausted();
});
assert.eq(resumeStream.getResumeToken(), resumeTokenInvalidate);

// Test resuming the change stream from the collection drop using 'startAfter'.
assertResumeExpected({
    coll: coll.getName(),
    spec: {startAfter: resumeTokenDrop},
    expected: [{operationType: "invalidate"}]
});

// Test resuming the change stream from the 'invalidate' notification using 'startAfter'.
cursor = cst.startWatchingChanges({
    collection: coll,
    pipeline: [{$changeStream: {startAfter: resumeTokenInvalidate}}],
    aggregateOptions: {collation: {locale: "simple"}, cursor: {batchSize: 0}}
});
cst.consumeDropUpTo({
    cursor: cursor,
    dropType: "drop",
    expectedNext: {
        operationType: "insert",
        ns: {db: db.getName(), coll: coll.getName()},
        fullDocument: {_id: "after recreate"},
        documentKey: {_id: "after recreate"}
    },
});

// Test that renaming a collection being watched generates a "rename" entry followed by an
// "invalidate". This is true if the change stream is on the source or target collection of the
// rename. Sharded collections cannot be renamed.
if (!FixtureHelpers.isSharded(coll)) {
    cursor = cst.startWatchingChanges({collection: collName, pipeline: [{$changeStream: {}}]});
    assertDropCollection(db, "renamed_coll");
    assert.commandWorked(coll.renameCollection("renamed_coll"));
    expectedChanges = [
        {
            operationType: "rename",
            ns: {db: db.getName(), coll: collName},
            to: {db: db.getName(), coll: "renamed_coll"},
        },
        {operationType: "invalidate"}
    ];
    cst.assertNextChangesEqual(
        {cursor: cursor, expectedChanges: expectedChanges, expectInvalidate: true});

    coll = db["renamed_coll"];

    // Repeat the test, this time with a change stream open on the target.
    cursor = cst.startWatchingChanges({collection: collName, pipeline: [{$changeStream: {}}]});
    assert.commandWorked(coll.renameCollection(collName));
    expectedChanges = [
        {
            operationType: "rename",
            ns: {db: db.getName(), coll: "renamed_coll"},
            to: {db: db.getName(), coll: collName},
        },
        {operationType: "invalidate"}
    ];
    const changes = cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expectedChanges});
    const resumeTokenRename = changes[0]._id;
    const resumeTokenInvalidate = changes[1]._id;

    coll = db[collName];
    assert.commandWorked(coll.insert({_id: "after rename"}));

    // Test resuming the change stream from the collection rename using 'resumeAfter'.
    assertResumeExpected({
        coll: coll.getName(),
        spec: {resumeAfter: resumeTokenRename},
        expected: [{operationType: "invalidate"}]
    });
    // Test resuming the change stream from the invalidate after the rename using 'resumeAfter'.
    assert.commandFailedWithCode(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeTokenInvalidate}}],
        cursor: {},
        collation: {locale: "simple"},
    }),
                                 ErrorCodes.InvalidResumeToken);

    // Test resuming the change stream from the rename using 'startAfter'.
    assertResumeExpected({
        coll: coll.getName(),
        spec: {startAfter: resumeTokenRename},
        expected: [{operationType: "invalidate"}]
    });

    // Test resuming the change stream from the invalidate after the rename using 'startAfter'.
    expectedChanges = [{
        operationType: "insert",
        ns: {db: db.getName(), coll: coll.getName()},
        fullDocument: {_id: "after rename"},
        documentKey: {_id: "after rename"}
    }];
    assertResumeExpected({
        coll: coll.getName(),
        spec: {startAfter: resumeTokenInvalidate},
        expected: expectedChanges
    });

    assertDropAndRecreateCollection(db, "renamed_coll");
    assert.commandWorked(db.renamed_coll.insert({_id: 0}));

    // Repeat the test again, this time using the 'dropTarget' option with an existing target
    // collection.
    cursor =
        cst.startWatchingChanges({collection: "renamed_coll", pipeline: [{$changeStream: {}}]});
    assert.commandWorked(coll.renameCollection("renamed_coll", true /* dropTarget */));
    expectedChanges = [
        {
            operationType: "rename",
            ns: {db: db.getName(), coll: collName},
            to: {db: db.getName(), coll: "renamed_coll"},
        },
        {operationType: "invalidate"}
    ];
    cst.assertNextChangesEqual(
        {cursor: cursor, expectedChanges: expectedChanges, expectInvalidate: true});

    coll = db["renamed_coll"];

    // Test the behavior of a change stream watching the target collection of a $out aggregation
    // stage.
    cursor = cst.startWatchingChanges({collection: collName, pipeline: [{$changeStream: {}}]});
    coll.aggregate([{$out: collName}]);
    // Note that $out will first create a temp collection, and then rename the temp collection
    // to the target. Do not explicitly check the 'ns' field.
    const rename = cst.getOneChange(cursor);
    assert.eq(rename.operationType, "rename", tojson(rename));
    assert.eq(rename.to, {db: db.getName(), coll: collName}, tojson(rename));
    assert.eq(cst.getOneChange(cursor, true).operationType, "invalidate");
}

// Test that dropping a database will first drop all of it's collections, invalidating any
// change streams on those collections.
cursor = cst.startWatchingChanges({
    collection: coll.getName(),
    pipeline: [{$changeStream: {}}],
});
assert.commandWorked(db.dropDatabase());

expectedChanges = [
    {
        operationType: "drop",
        ns: {db: db.getName(), coll: coll.getName()},
    },
    {operationType: "invalidate"}
];
cst.assertNextChangesEqual(
    {cursor: cursor, expectedChanges: expectedChanges, expectInvalidate: true});

cst.cleanUp();
}());
