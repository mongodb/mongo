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
    "arrayForResize": [kLargeStr, 1],
    obj: {
        'sub.obj': {'d.o.t.t.e.d.a.r.r.a.y..': [[{a: {'b.c': 1, field: kLargeStr}}, "truncated"]]}
    },
    'd.o.t.t.e.d.o.b.j.': {'sub.obj': {'b.c': 2}},
    'objectWithNumericField': {'0': {'1': 'numeric', field: kLargeStr}},
    "arrayWithNumericField": [[{'0': "numeric", a: {'b.c': 1}, field: kLargeStr}]],
    "arrayWithDotted.AndNumericFields": [[{'0': [{'1.2': {'a.b': null, c: kLargeStr}}]}]],
}));

const changeStreamCursor = cst.startWatchingChanges(
    {pipeline: [{$changeStream: {showExpandedEvents: true}}], collection: db.coll});

// Test that a path which only contains non-dotted fields and array indices is not reported under
// 'disambiguatedPaths'.
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
        disambiguatedPaths: {}
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Tests that an update modifying a non-array numeric field name is reported as a string rather than
// as an integer under 'disambiguatedPaths'. Array indexes are reported as integers.
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
        disambiguatedPaths: {"arrayWithNumericField.0.0.1": ["arrayWithNumericField", 0, 0, "1"]}
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Tests that an update modifying a non-array numeric field name is reported when no array indices
// or dotted fields are present.
assert.commandWorked(db.coll.update({_id: 100}, {$set: {"objectWithNumericField.0.1": "updated"}}));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "coll"},
    operationType: "update",
    updateDescription: {
        updatedFields: {"objectWithNumericField.0.1": "updated"},
        removedFields: [],
        truncatedArrays: [],
        disambiguatedPaths: {"objectWithNumericField.0.1": ["objectWithNumericField", "0", "1"]}
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Tests that an update with $unset array does not report the array under 'disambiguatedPaths'.
assert.commandWorked(db.coll.update({_id: 100}, [{$unset: ["arrayForReplacement"]}]));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "coll"},
    operationType: "update",
    updateDescription: {
        updatedFields: {},
        removedFields: ["arrayForReplacement"],
        truncatedArrays: [],
        disambiguatedPaths: {}
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Tests that an update with 'truncatedArrays' does not report the array under 'disambiguatedPaths'.
assert.commandWorked(db.coll.update({_id: 100}, [
    {$replaceWith: {$setField: {field: "arrayForResize", input: '$$ROOT', value: [kLargeStr]}}},
]));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "coll"},
    operationType: "update",
    updateDescription: {
        updatedFields: {},
        removedFields: [],
        truncatedArrays: [{field: "arrayForResize", newSize: 1}],
        disambiguatedPaths: {}
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Verify that top-level dotted fields are reported under 'disambiguatedPaths'.
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
        disambiguatedPaths:
            {"d.o.t.t.e.d.o.b.j.": ["d.o.t.t.e.d.o.b.j."], "new.Field.": ["new.Field."]}
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Test that a combination of dotted fields and array indices are reported in 'disambiguatedPaths'.
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
        disambiguatedPaths: {
            "obj.sub.obj.d.o.t.t.e.d.a.r.r.a.y...0":
                ["obj", "sub.obj", "d.o.t.t.e.d.a.r.r.a.y..", 0],
            "obj.sub.obj.d.o.t.t.e.d.a.r.r.a.y...0.0.a.b.c":
                ["obj", "sub.obj", "d.o.t.t.e.d.a.r.r.a.y..", 0, 0, "a", "b.c"],
        }
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

// Test that an update which modifies a path containing dotted, numeric and array index fields
// distinguishes all three in 'disambiguatedPaths'.
assert.commandWorked(
    db.coll.update({_id: 100}, [{
                       $replaceWith: {
                           $setField: {
                               field: "arrayWithDotted.AndNumericFields",
                               input: '$$ROOT',
                               value: {$literal: [[{'0': [{'1.2': {'a.b': true, c: kLargeStr}}]}]]}
                           }
                       }
                   }]));
expected = {
    documentKey: {_id: 100},
    ns: {db: "test", coll: "coll"},
    operationType: "update",
    updateDescription: {
        updatedFields: {"arrayWithDotted.AndNumericFields.0.0.0.0.1.2.a.b": true},
        removedFields: [],
        truncatedArrays: [],
        disambiguatedPaths: {
            "arrayWithDotted.AndNumericFields.0.0.0.0.1.2.a.b":
                ["arrayWithDotted.AndNumericFields", 0, 0, "0", 0, "1.2", "a.b"]
        }
    },
};
cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: [expected]});

cst.cleanUp();
}());
