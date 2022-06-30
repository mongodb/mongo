/**
 * Tests the behavior of change streams in the presence of 'showSystemEvents' flag.
 *
 * @tags: [
 *  requires_fcv_60,
 *  # This test assumes certain ordering of events.
 *  assumes_unsharded_collection,
 *  # Assumes to implicit index creation.
 *  assumes_no_implicit_index_creation
 * ]
 */
(function() {
"use strict";

load('jstests/libs/change_stream_util.js');        // For 'ChangeStreamTest' and
                                                   // 'assertChangeStreamEventEq'.
load('jstests/libs/collection_drop_recreate.js');  // For 'assertDropCollection'.

const testDB = db.getSiblingDB(jsTestName());

// Assert that the flag is not allowed with 'apiStrict'.
assert.commandFailedWithCode(testDB.runCommand({
    aggregate: 1,
    pipeline: [{$changeStream: {showSystemEvents: true}}],
    cursor: {},
    apiVersion: "1",
    apiStrict: true
}),
                             ErrorCodes.APIStrictError);

const test = new ChangeStreamTest(testDB);

const systemNS = {
    db: testDB.getName(),
    coll: 'system.js'
};
const collRenamed = 'collRenamed';

function runWholeDbChangeStreamTestWithoutSystemEvents(test, cursor, nonSystemColl) {
    assertDropCollection(testDB, nonSystemColl.getName());

    let expected = {
        ns: {db: testDB.getName(), coll: nonSystemColl.getName()},
        operationType: "drop",
    };
    test.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Write to the 'normal' collection.
    assert.commandWorked(nonSystemColl.insert({_id: 1, a: 1}));

    // Insert a document into the system.js collection.
    assert.commandWorked(testDB.system.js.insert({_id: 3, c: 1}));

    // The next event should still be only the insert into the 'regular' collection, even though
    // we've inserted into the system collection.
    let expectedChanges = [
        {
            ns: {db: testDB.getName(), coll: nonSystemColl.getName()},
            operationType: "create",
        },
        {
            documentKey: {_id: 1},
            fullDocument: {_id: 1, a: 1},
            ns: {db: testDB.getName(), coll: nonSystemColl.getName()},
            operationType: "insert",
        }
    ];
    test.assertNextChangesEqual({cursor: cursor, expectedChanges: expectedChanges});

    // Update into a system collection.
    assert.commandWorked(testDB.system.js.update({_id: 3}, {c: 2}));
    // Delete from a system collection.
    assert.commandWorked(testDB.system.js.remove({_id: 3}));

    // We don't see any of the preceding CRUD operations on the system collection.

    // Once again write to the 'normal' collection.
    assert.commandWorked(nonSystemColl.insert({_id: 2, a: 1}));

    // Similar as to before, the next event should be the insert on the 'regular' collection even
    // though we have performed a number of operations on the system collection.
    expected = {
        documentKey: {_id: 2},
        fullDocument: {_id: 2, a: 1},
        ns: {db: testDB.getName(), coll: nonSystemColl.getName()},
        operationType: "insert",
    };
    test.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});
}

function runWholeDbChangeStreamTestWithSystemEvents(test, cursor, nonSystemColl) {
    assertDropCollection(testDB, nonSystemColl.getName());

    let expected = {
        ns: {db: testDB.getName(), coll: nonSystemColl.getName()},
        operationType: "drop",
    };
    test.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Write to the 'normal' collection.
    assert.commandWorked(nonSystemColl.insert({_id: 1, a: 1}));

    let expectedChanges = [
        {
            ns: {db: testDB.getName(), coll: nonSystemColl.getName()},
            operationType: "create",
        },
        {
            documentKey: {_id: 1},
            fullDocument: {_id: 1, a: 1},
            ns: {db: testDB.getName(), coll: nonSystemColl.getName()},
            operationType: "insert",
        }
    ];
    test.assertNextChangesEqual({cursor: cursor, expectedChanges: expectedChanges});

    // Insert into a system collection.
    assert.commandWorked(testDB.system.js.insert({_id: 2, b: 1}));

    expected = {
        documentKey: {_id: 2},
        fullDocument: {_id: 2, b: 1},
        ns: {db: testDB.getName(), coll: systemNS.coll},
        operationType: "insert",
    };
    test.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Update into a system collection.
    assert.commandWorked(testDB.system.js.update({_id: 2}, {b: 2}));

    expected = {
        documentKey: {_id: 2},
        fullDocument: {_id: 2, b: 2},
        ns: {db: testDB.getName(), coll: systemNS.coll},
        operationType: "replace",
    };
    test.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Delete from a system collection.
    assert.commandWorked(testDB.system.js.remove({_id: 2}));

    expected = {
        documentKey: {_id: 2},
        ns: {db: testDB.getName(), coll: systemNS.coll},
        operationType: "delete",
    };
    test.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});
}

function runSingleCollectionChangeStreamTest(test, cursor, nonSystemColl) {
    // Write to the 'normal' collection.
    assert.commandWorked(nonSystemColl.insert({_id: 1, a: 1}));

    let expected = {
        ns: {db: testDB.getName(), coll: nonSystemColl.getName()},
        operationType: "create",
    };
    test.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Insert into a system collection.
    assert.commandWorked(testDB.system.js.insert({_id: 1, a: 1}));
    // Update into a system collection.
    assert.commandWorked(testDB.system.js.update({_id: 1}, {a: 2}));
    // Delete from a system collection.
    assert.commandWorked(testDB.system.js.remove({_id: 1}));

    // Write again to the 'normal' collection as a sentinel write.
    assert.commandWorked(nonSystemColl.insert({_id: 2, a: 2}));

    // The only expected events should be the two inserts into the non-system collection.
    const expectedChanges = [
        {
            documentKey: {_id: 1},
            fullDocument: {_id: 1, a: 1},
            ns: {db: testDB.getName(), coll: nonSystemColl.getName()},
            operationType: "insert",
        },
        {
            documentKey: {_id: 2},
            fullDocument: {_id: 2, a: 2},
            ns: {db: testDB.getName(), coll: nonSystemColl.getName()},
            operationType: "insert",
        }
    ];
    test.assertNextChangesEqual({cursor: cursor, expectedChanges: expectedChanges});
}

const regularColl = testDB.test_coll;
regularColl.drop();

// Run a single-collection stream on a normal collection with 'showSystemEvents' set to 'true'.
let pipeline = [{$changeStream: {showExpandedEvents: true, showSystemEvents: true}}];
let cursor = test.startWatchingChanges({pipeline: pipeline, collection: regularColl});
runSingleCollectionChangeStreamTest(test, cursor, regularColl);

// Run a whole-DB stream with 'showSystemEvents' set to 'true'.
pipeline = [{$changeStream: {showExpandedEvents: true, showSystemEvents: true}}];
cursor = test.startWatchingChanges({pipeline: pipeline, collection: 1});
runWholeDbChangeStreamTestWithSystemEvents(test, cursor, regularColl);

// Now run a whole-DB stream with 'showSystemEvents' set to 'false'.
pipeline = [{$changeStream: {showExpandedEvents: true, showSystemEvents: false}}];
cursor = test.startWatchingChanges({pipeline: pipeline, collection: 1});
runWholeDbChangeStreamTestWithoutSystemEvents(test, cursor, regularColl);
}());
