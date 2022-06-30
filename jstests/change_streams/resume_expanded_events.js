/**
 * Tests the behavior of resuming a change stream with the resume token generated for newly added
 * events.
 *
 *  @tags: [
 *   requires_fcv_60,
 *   # The test assumes certain ordering of the events. The chunk migrations on a sharded collection
 *   # could break the test.
 *   assumes_unsharded_collection,
 *   assumes_against_mongod_not_mongos,
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

    // Test the 'create' event.
    assert.commandWorked(testDB.createCollection(collName));
    const createEvent = test.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: {
            operationType: "create",
            ns: ns,
            operationDescription: {idIndex: {v: 2, key: {_id: 1}, name: "_id_"}}
        }
    })[0];

    // Test the 'createIndexes' event on an empty collection.
    assert.commandWorked(testDB[collName].createIndex({a: 1}));
    const createIndexesEvent1 = test.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: {
            operationType: "createIndexes",
            ns: ns,
            operationDescription: {indexes: [{v: 2, key: {a: 1}, name: "a_1"}]}
        }
    })[0];

    // Insert a document so that the collection is not empty so that we can get coverage for
    // 'commitIndexBuild' when creating an index on field "b" below.
    assert.commandWorked(testDB[collName].insert({_id: 1, a: 1, b: 1}));
    const insertEvent1 = test.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: {
            operationType: "insert",
            ns: ns,
            fullDocument: {_id: 1, a: 1, b: 1},
            documentKey: {_id: 1},
        }
    })[0];

    // Test the 'createIndexes' event on a non-empty collection.
    assert.commandWorked(testDB[collName].createIndex({b: -1}));
    const createIndexesEvent2 = test.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: {
            operationType: "createIndexes",
            ns: ns,
            operationDescription: {indexes: [{v: 2, key: {b: -1}, name: "b_-1"}]}
        }
    })[0];

    // Test the 'dropIndexes' event.
    assert.commandWorked(testDB[collName].dropIndex({b: -1}));
    const dropIndexesEvent = test.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: {
            operationType: "dropIndexes",
            ns: ns,
            operationDescription: {indexes: [{v: 2, key: {b: -1}, name: "b_-1"}]}
        }
    })[0];

    // Insert another document so that we can validate the resuming behavior for the
    // dropIndexes event.
    assert.commandWorked(testDB[collName].insert({_id: 2, a: 2, b: 2}));
    const insertEvent2 = test.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: {
            operationType: "insert",
            ns: ns,
            fullDocument: {_id: 2, a: 2, b: 2},
            documentKey: {_id: 2},
        }
    })[0];

    function testResume(resumeOption) {
        function testResumeForEvent(event, nextEventDesc) {
            pipeline = [{$changeStream: {showExpandedEvents: true, [resumeOption]: event._id}}];
            cursor = test.startWatchingChanges({pipeline, collection: collNameForChangeStream});
            test.assertNextChangesEqual({cursor: cursor, expectedChanges: nextEventDesc});
        }

        testResumeForEvent(createEvent, createIndexesEvent1);
        testResumeForEvent(createIndexesEvent1, insertEvent1);
        testResumeForEvent(createIndexesEvent2, dropIndexesEvent);
        testResumeForEvent(dropIndexesEvent, insertEvent2);
    }

    // Testing resuming with 'resumeAfter' and 'startAfter'.
    testResume("resumeAfter");
    testResume("startAfter");

    testDB[collName].drop();
}

runTest(1);  // Runs the test using a whole-db change stream
runTest(collName);
}());
