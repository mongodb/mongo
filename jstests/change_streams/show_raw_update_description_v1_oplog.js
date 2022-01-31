/**
 * Tests that change streams with the 'showRawUpdateDescription' option enabled will return update
 * events with the 'rawUpdateDescription' field instead of the 'updateDescription' field, and tests
 * that the 'showRawUpdateDescription' option has no effect on replacements or other types of
 * events.
 */
(function() {
"use strict";

load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.
load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

// Drop and recreate the collections to be used in this set of tests.
assertDropAndRecreateCollection(db, "t1");

assert.commandWorked(db.t1.insert([
    {_id: 3, a: 5, b: 1},
    {_id: 4, a: 0, b: 1},
    {_id: 5, a: 0, b: 1},
    {_id: 6, a: 1, b: 1},
    {_id: 7, a: 1, b: 1},
    {_id: 8, a: 2, b: {c: 1}}
]));

const cst = new ChangeStreamTest(db);
let cursor = cst.startWatchingChanges(
    {pipeline: [{$changeStream: {showRawUpdateDescription: true}}], collection: db.t1});

//
// Test insert, replace, and delete operations and verify the corresponding change stream events
// are unaffected by the 'showRawUpdateDescription' option.
//
jsTestLog("Testing insert");
assert.commandWorked(db.t1.insert({_id: 1, a: 1}));
let expected = {
    documentKey: {_id: 1},
    fullDocument: {_id: 1, a: 1},
    ns: {db: "test", coll: "t1"},
    operationType: "insert",
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

jsTestLog("Testing upsert");
assert.commandWorked(db.t1.update({_id: 2}, {_id: 2, a: 4}, {upsert: true}));
expected = {
    documentKey: {_id: 2},
    fullDocument: {_id: 2, a: 4},
    ns: {db: "test", coll: "t1"},
    operationType: "insert",
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

jsTestLog("Testing replacement");
assert.commandWorked(db.t1.update({_id: 1}, {_id: 1, a: 3}));
expected = {
    documentKey: {_id: 1},
    fullDocument: {_id: 1, a: 3},
    ns: {db: "test", coll: "t1"},
    operationType: "replace",
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

jsTestLog("Testing another replacement");
assert.commandWorked(db.t1.update({_id: 1}, {_id: 1, b: 3}));
expected = {
    documentKey: {_id: 1},
    fullDocument: {_id: 1, b: 3},
    ns: {db: "test", coll: "t1"},
    operationType: "replace",
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

jsTestLog("Testing delete");
assert.commandWorked(db.t1.remove({_id: 1}));
expected = {
    documentKey: {_id: 1},
    ns: {db: "test", coll: "t1"},
    operationType: "delete",
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

jsTestLog("Testing delete with justOne:false");
assert.commandWorked(db.t1.remove({a: 1}, {justOne: false}));
expected = [
    {
        documentKey: {_id: 6},
        ns: {db: "test", coll: "t1"},
        operationType: "delete",
    },
    {
        documentKey: {_id: 7},
        ns: {db: "test", coll: "t1"},
        operationType: "delete",
    }
];
cst.assertNextChangesEqualUnordered({cursor: cursor, expectedChanges: expected});

//
// The remainder of the test-cases below exercise various update scenarios that produce
// 'rawUpdateDescription'.
//

//
// Test op-style updates.
//
jsTestLog("Testing op-style update with $inc");
assert.commandWorked(db.t1.update({_id: 3}, {$inc: {b: 2}}));
expected = {
    documentKey: {_id: 3},
    ns: {db: "test", coll: "t1"},
    operationType: "update",
    rawUpdateDescription: {"$v": 1, "$set": {b: 3}}
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

jsTestLog("Testing op-style update with $set and multi:true");
assert.commandWorked(db.t1.update({a: 0}, {$set: {b: 2}}, {multi: true}));
expected = [
    {
        documentKey: {_id: 4},
        ns: {db: "test", coll: "t1"},
        operationType: "update",
        rawUpdateDescription: {"$v": 1, "$set": {b: 2}}
    },
    {
        documentKey: {_id: 5},
        ns: {db: "test", coll: "t1"},
        operationType: "update",
        rawUpdateDescription: {"$v": 1, "$set": {b: 2}}
    }
];
cst.assertNextChangesEqualUnordered({cursor: cursor, expectedChanges: expected});

jsTestLog("Testing op-style update with $unset");
assert.commandWorked(db.t1.update({_id: 3}, {$unset: {b: ""}}));
expected = {
    documentKey: {_id: 3},
    ns: {db: "test", coll: "t1"},
    operationType: "update",
    rawUpdateDescription: {"$v": 1, "$unset": {b: true}}
};
cst.assertNextChangesEqualUnordered({cursor: cursor, expectedChanges: expected});

jsTestLog("Testing op-style update with $set on nested field");
assert.commandWorked(db.t1.update({_id: 8}, {$set: {"b.d": 2}}));
expected = {
    documentKey: {_id: 8},
    ns: {db: "test", coll: "t1"},
    operationType: "update",
    rawUpdateDescription: {"$v": 1, "$set": {"b.d": 2}}
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

cst.cleanUp();
}());
