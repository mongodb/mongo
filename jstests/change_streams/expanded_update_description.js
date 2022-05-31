/**
 * Test change stream 'updateDescription' with 'showExpandedEvents'.
 *
 * @tags: [
 *      requires_fcv_61,
 *      featureFlagChangeStreamsFurtherEnrichedEvents,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

// Drop and recreate the collections to be used in this set of tests.
assertDropAndRecreateCollection(db, "coll");

const cst = new ChangeStreamTest(db);

const kLargeStr = '*'.repeat(128);
assert.commandWorked(db.coll.insert({
    _id: 100,
    "topLevelArray": [{subArray: [0, [0, [{bottomArray: [1, 2, kLargeStr]}]], 2, 3, kLargeStr]}],
    "arrayForReplacement": [0, 1, 2, 3],
    "array.For.Resize": [kLargeStr, 1],
    obj: {
        'sub.obj': {'d.o.t.t.e.d.a.r.r.a.y..': [[{a: {'b.c': 1, field: kLargeStr}}, "truncated"]]}
    },
    'd.o.t.t.e.d.o.b.j.': {'sub.obj': {'b.c': 2}},
    "arrayWithNumericField": [[{'0': "numeric", a: {'b.c': 1}, field: kLargeStr}]],
}));

const changeStreamCursor = cst.startWatchingChanges(
    {pipeline: [{$changeStream: {showExpandedEvents: true}}], collection: db.coll});

// Test to verify that 'specialFields.arrayIndices' reports all the arrayIndices along a path in the
// presence of nested arrays.
assert.commandWorked(db.coll.update({_id: 100}, {
    $set: {"a": 2, "topLevelArray.0.subArray.1.1.0.bottomArray.2": 3, "arrayForReplacement": [0]}
}));

let expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "coll"},
    operationType: "update",
    updateDescription: {
        updatedFields:
            {"arrayForReplacement": [0], "a": 2, "topLevelArray.0.subArray.1.1.0.bottomArray.2": 3},
        removedFields: [],
        truncatedArrays: [],
        specialFields: {
            arrayIndices: {
                "topLevelArray": [0],
                "topLevelArray.0.subArray": [1],
                "topLevelArray.0.subArray.1": [1],
                "topLevelArray.0.subArray.1.1": [0],
                "topLevelArray.0.subArray.1.1.0.bottomArray": [2],
            },
            dottedFields: {}
        }
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Tests that an update modifying multiple array elements are all reported under
// 'specialFields.arrayIndices'.
assert.commandWorked(db.coll.update(
    {_id: 100}, {$set: {"topLevelArray.0.subArray.2": 4, "topLevelArray.0.subArray.3": 5}}));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "coll"},
    operationType: "update",
    updateDescription: {
        updatedFields: {"topLevelArray.0.subArray.2": 4, "topLevelArray.0.subArray.3": 5},
        removedFields: [],
        truncatedArrays: [],
        specialFields: {
            arrayIndices: {topLevelArray: [0], "topLevelArray.0.subArray": [2, 3]},
            dottedFields: {}
        }
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Tests that an update modifying a non-array numeric field name is NOT reported under
// 'specialFields.arrayIndices'.
assert.commandWorked(
    db.coll.update({_id: 100}, {$set: {"arrayWithNumericField.0.0.1": {"b.c": 1}}}));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "coll"},
    operationType: "update",
    updateDescription: {
        updatedFields: {"arrayWithNumericField.0.0.1": {"b.c": 1}},
        removedFields: [],
        truncatedArrays: [],
        specialFields: {
            arrayIndices: {arrayWithNumericField: [0], "arrayWithNumericField.0": [0]},
            dottedFields: {}
        }
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Tests that an update with $unset array does not report the array under
// 'specialFields.arrayIndices'.
assert.commandWorked(db.coll.update({_id: 100}, [{$unset: ["arrayForReplacement"]}]));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "coll"},
    operationType: "update",
    updateDescription: {
        updatedFields: {},
        removedFields: ["arrayForReplacement"],
        truncatedArrays: [],
        specialFields: {arrayIndices: {}, dottedFields: {}}
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Tests that an update with 'truncatedArrays' does not report the array under
// 'specialFields.arrayIndices'.
assert.commandWorked(db.coll.update({_id: 100}, [
    {$replaceWith: {$setField: {field: "array.For.Resize", input: '$$ROOT', value: [kLargeStr]}}},
]));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "coll"},
    operationType: "update",
    updateDescription: {
        updatedFields: {},
        removedFields: [],
        truncatedArrays: [{field: "array.For.Resize", newSize: 1}],
        specialFields: {arrayIndices: {}, dottedFields: {"": ["array.For.Resize"]}}
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Verify that the top-level dotted fields are reported with empty path-prefix under
// 'specialFields.dottedFields'.
assert.commandWorked(db.coll.update({_id: 100}, [
    {
        $replaceWith:
            {$setField: {field: "d.o.t.t.e.d.o.b.j.", input: '$$ROOT', value: {'subObj': 1}}}
    },
    {$replaceWith: {$setField: {field: "new.Field.", input: '$$ROOT', value: 1}}}
]));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "coll"},
    operationType: "update",
    updateDescription: {
        updatedFields: {"d.o.t.t.e.d.o.b.j.": {subObj: 1}, "new.Field.": 1},
        removedFields: [],
        truncatedArrays: [],
        specialFields: {arrayIndices: {}, dottedFields: {"": ["d.o.t.t.e.d.o.b.j.", "new.Field."]}}
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Verify that a dotted fields can be reported under both 'specialFields.arrayIndices' and
// 'specialFields.dottedFields'.
assert.commandWorked(db.coll.update(
    {_id: 100}, [{
        $set: {
            obj: {
                $setField: {
                    field: "sub.obj",
                    input: '$obj',
                    value: {
                        $literal: {'d.o.t.t.e.d.a.r.r.a.y..': [[{a: {'b.c': 2, field: kLargeStr}}]]}
                    }
                }
            }
        }
    }]));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "coll"},
    operationType: "update",
    updateDescription: {
        updatedFields: {"obj.sub.obj.d.o.t.t.e.d.a.r.r.a.y...0.0.a.b.c": 2},
        removedFields: [],
        truncatedArrays: [{field: "obj.sub.obj.d.o.t.t.e.d.a.r.r.a.y...0", newSize: 1}],
        specialFields: {
            arrayIndices: {
                "obj.sub.obj.d.o.t.t.e.d.a.r.r.a.y..": [0],
                "obj.sub.obj.d.o.t.t.e.d.a.r.r.a.y...0": [0]
            },
            dottedFields: {
                "obj.sub.obj.d.o.t.t.e.d.a.r.r.a.y...0.0.a": ["b.c"],
                "obj.sub.obj": ["d.o.t.t.e.d.a.r.r.a.y.."],
                obj: ["sub.obj"]
            }
        }
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

cst.cleanUp();
}());
