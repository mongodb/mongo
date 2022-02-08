/**
 * Tests the behavior of resuming a change stream with the resume token generated for newly added
 * events.
 *
 *  @tags: [
 *   requires_fcv_60,
 *   # The test assumes certain ordering of the events. The chunk migrations on a sharded collection
 *   # could break the test.
 *   assumes_unsharded_collection,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/collection_drop_recreate.js');  // For 'assertDropAndRecreateCollection' and
                                                   // 'assertDropCollection'.
load('jstests/libs/change_stream_util.js');        // For 'ChangeStreamTest' and
                                                   // 'assertChangeStreamEventEq'.

const testDB = db.getSiblingDB(jsTestName());
const collName = "coll1";
if (!isChangeStreamsVisibilityEnabled(testDB)) {
    return;
}

const test = new ChangeStreamTest(testDB);
const ns = {
    db: jsTestName(),
    coll: collName
};

function runTest(collNameForChangeStream) {
    let pipeline = [{$changeStream: {showExpandedEvents: true}}];
    let cursor = test.startWatchingChanges({
        pipeline,
        collection: collNameForChangeStream,
        aggregateOptions: {cursor: {batchSize: 0}}
    });

    //
    // For 'create' event.
    //
    assert.commandWorked(testDB.createCollection(collName));
    const createEvent = test.getOneChange(cursor);

    assertChangeStreamEventEq(createEvent, {
        operationType: "create",
        ns: ns,
        operationDescription: {idIndex: {v: 2, key: {_id: 1}, name: "_id_"}}
    });

    // Insert a document before starting the next change stream so that we can validate the resuming
    // behavior.
    assert.commandWorked(testDB[collName].insert({_id: 1}));

    // Resume with 'resumeAfter'.
    pipeline = [{$changeStream: {showExpandedEvents: true, resumeAfter: createEvent._id}}];
    cursor = test.startWatchingChanges({pipeline, collection: collNameForChangeStream});

    test.assertNextChangesEqual({
        cursor,
        expectedChanges: {
            operationType: "insert",
            ns: ns,
            fullDocument: {_id: 1},
            documentKey: {_id: 1},
        }
    });

    // Resume with 'startAfter'.
    pipeline = [{$changeStream: {showExpandedEvents: true, startAfter: createEvent._id}}];
    cursor = test.startWatchingChanges({pipeline, collection: collNameForChangeStream});

    test.assertNextChangesEqual({
        cursor,
        expectedChanges: {
            operationType: "insert",
            ns: ns,
            fullDocument: {_id: 1},
            documentKey: {_id: 1},
        }
    });

    testDB[collName].drop();
}

runTest(1);  // Runs the test using a whole-db change stream
runTest(collName);
}());