/**
 * Test pipeline-style updates with delta oplog entries.
 *
 * @tags: [requires_fcv_47]
 */

(function() {
"use strict";

load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

// Drop and recreate the collections to be used in this set of tests.
assertDropAndRecreateCollection(db, "t1");

const cst = new ChangeStreamTest(db);

jsTestLog("Testing pipeline-based update with $set");
const kGiantStr = '*'.repeat(1024);
const kMediumStr = '*'.repeat(128);
const kSmallStr = '*'.repeat(32);

assert.commandWorked(db.t1.insert({
    _id: 100,
    "a": 1,
    "b": 2,
    "arrayForSubdiff": [kGiantStr, {a: kMediumStr}, 1, 2, 3],
    "arrayForReplacement": [0, 1, 2, 3],
    "giantStr": kGiantStr
}));

const changeStreamCursor =
    cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
// Test change stream with 'fullDocument' set.
const fullDocCursor = cst.startWatchingChanges(
    {pipeline: [{$changeStream: {fullDocument: "updateLookup"}}], collection: db.t1});

assert.commandWorked(db.t1.update({_id: 100}, [{
                                      $set: {
                                          "a": 2,
                                          "arrayForSubdiff": [kGiantStr, {"a": kMediumStr, "b": 3}],
                                          "arrayForReplacement": [0],
                                          "c": 3
                                      }
                                  }]));

let expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "t1"},
    operationType: "update",
    fullDocument: {
        "_id": 100,
        "a": 2,
        "b": 2,
        "arrayForSubdiff": [kGiantStr, {"a": kMediumStr, "b": 3}],
        "arrayForReplacement": [0],
        "giantStr": kGiantStr,
        "c": 3,
    },
    updateDescription: {
        updatedFields: {"a": 2, "c": 3, "arrayForReplacement": [0], "arrayForSubdiff.1.b": 3},
        removedFields: [],
        // Only records field 'arrayForSubdiff' here even though the size of field
        // 'arrayForReplacement' was also changed, as we expect the diff in 'arrayForReplacement'
        // to be replacement type format.
        truncatedArrays: [{"field": "arrayForSubdiff", "newSize": 2}],
    },
};
cst.assertNextChangesEqual({cursor: fullDocCursor, expectedChanges: [expected]});

delete expected.fullDocument;
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

jsTestLog("Testing pipeline-based update with $unset");
// Pre-image: {
//  _id: 100, "a": 2, "b": 2, "c": 3,
//  "arrayForReplacement": [0],
//  "arrayForSubdiff": [kGiantStr, {"a": kMediumStr, "b": 3}],
//  "giantStr": kGiantStr,
//  }.
assert.commandWorked(db.t1.update({_id: 100}, [{$unset: ["a"]}]));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "t1"},
    operationType: "update",
    updateDescription: {
        updatedFields: {},
        removedFields: ["a"],
        truncatedArrays: [],
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

jsTestLog("Testing pipeline-based update with $replaceRoot");
// Pre-image: {
//  _id: 100, "c": 3, "b": 2,
//  "arrayForReplacement": [0],
//  "arrayForSubdiff": [kGiantStr, {"a": kMediumStr, "b": 3}],
//  "giantStr": kGiantStr,
//  }.
assert.commandWorked(
    db.t1.update({_id: 100}, [{$replaceRoot: {newRoot: {_id: 100, "giantStr": kGiantStr}}}]));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "t1"},
    operationType: "update",
    updateDescription: {
        updatedFields: {},
        removedFields: ["c", "arrayForReplacement", "arrayForSubdiff", "b"],
        truncatedArrays: [],
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

jsTestLog("Testing pipeline-based update with a complex pipeline");
// Pre-image: {
//  _id: 100,
//  "giantStr": kGiantStr,
//  }.

assert.commandWorked(db.t1.update({_id: 100}, [
    {
        $replaceRoot: {
            // Also constructing a new doc for later test.
            newRoot: {
                _id: 100,
                "giantStr": kGiantStr,
                "arr": [{"x": 1, "y": kSmallStr}, kMediumStr],
                "arr_a": [1, kMediumStr],
                "arr_b": [[1, kSmallStr], kMediumStr],
                "arr_c": [[kSmallStr, 1, 2, 3], kMediumStr],
                "obj": {"x": {"a": 1, "b": 1, "c": [kMediumStr, 1, 2, 3], "str": kMediumStr}},
            }
        }
    },
    {$addFields: {"a": "updated", "b": 2, "doc": {"a": {0: "foo"}}}},
    {
        $project: {
            "a": true,
            "giantStr": true,
            "doc": true,
            "arr": true,
            "arr_a": true,
            "arr_b": true,
            "arr_c": true,
            "obj": true
        }
    },
]));

expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "t1"},
    operationType: "update",
    updateDescription: {
        updatedFields: {
            "a": "updated",
            "doc": {"a": {0: "foo"}},
            "arr": [{"x": 1, "y": kSmallStr}, kMediumStr],
            "arr_a": [1, kMediumStr],
            "arr_b": [[1, kSmallStr], kMediumStr],
            "arr_c": [[kSmallStr, 1, 2, 3], kMediumStr],
            "obj": {"x": {"a": 1, "b": 1, "c": [kMediumStr, 1, 2, 3], "str": kMediumStr}},
        },
        removedFields: [],
        truncatedArrays: [],
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Pre-image: {
//  _id: 100,
//  "giantStr": kGiantStr,
//  "a": "updated",
//  "doc": {"a": {0: "foo"}},
//  "arr": [{"x": 1, "y": kSmallStr}, kMediumStr],
//  "arr_a": [1, kMediumStr],
//  "arr_b": [[1, kSmallStr], kMediumStr],
//  "arr_c": [[kSmallStr, 1, 2, 3], kMediumStr],
//  "obj": {"x": {"a": 1, "b": 1, "c": [kMediumStr, 1, 2, 3], "str": kMediumStr}},
//  }.
jsTestLog("Testing pipeline-based update with modifications to nested elements");

assert.commandWorked(
    db.t1.update({_id: 100}, [{
                     $replaceRoot: {
                         newRoot: {
                             _id: 100,
                             "giantStr": kGiantStr,
                             "arr": [{"y": kSmallStr}, kMediumStr],
                             "arr_a": [2, kMediumStr],
                             "arr_b": [[2, kSmallStr], kMediumStr],
                             "arr_c": [[kSmallStr], kMediumStr],
                             "obj": {"x": {"b": 2, "c": [kMediumStr], "str": kMediumStr}},
                         }
                     }
                 }]));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "t1"},
    operationType: "update",
    updateDescription: {
        updatedFields: {
            "arr_a.0": 2,
            "arr_b.0.0": 2,
            "obj.x.b": 2,
        },
        removedFields: ["a", "doc", "arr.0.x", "obj.x.a"],
        truncatedArrays: [{"field": "arr_c.0", "newSize": 1}, {"field": "obj.x.c", "newSize": 1}],
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

cst.cleanUp();
}());
