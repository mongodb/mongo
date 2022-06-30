/**
 * Tests that change streams with the 'showRawUpdateDescription' option enabled will return update
 * events with the 'rawUpdateDescription' field instead of the 'updateDescription' field, and tests
 * that the 'showRawUpdateDescription' option has no effect on replacements or other types of
 * events.
 * @tags: [
 *   requires_fcv_60,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");       // For arrayEq.
load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.
load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

// Drop and recreate the collections to be used in this set of tests.
assertDropAndRecreateCollection(db, "t1");
assertDropAndRecreateCollection(db, "t2");
assertDropAndRecreateCollection(db, "t1Copy");
assertDropAndRecreateCollection(db, "t2Copy");

assert.commandWorked(db.t1.insert([
    {_id: 3, a: 5, b: 1},
    {_id: 4, a: 0, b: 1},
    {_id: 5, a: 0, b: 1},
    {_id: 6, a: 1, b: 1},
    {_id: 7, a: 1, b: 1},
    {_id: 8, a: 2, b: {c: 1}}
]));

const kGiantStr = '*'.repeat(1024);
const kMediumStr = '*'.repeat(128);
const kSmallStr = '*'.repeat(32);

assert.commandWorked(db.t2.insert({
    _id: 100,
    "a": 1,
    "b": 2,
    "arrayForSubdiff": [kGiantStr, {a: kMediumStr}, 1, 2, 3],
    "arrayForReplacement": [0, 1, 2, 3],
    "giantStr": kGiantStr
}));

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

function assertCollectionsAreIdentical(coll1, coll2) {
    const values1 = coll1.find().toArray();
    const values2 = coll2.find().toArray();
    assert(arrayEq(values1, values2),
           () => "actual: " + tojson(values1) + "  expected: " + tojson(values2));
}

function assertCanApplyRawUpdate(origColl, copyColl, events) {
    if (!Array.isArray(events)) {
        events = [events];
    }
    for (let event of events) {
        assert.commandWorked(copyColl.update(
            event.documentKey,
            [{$_internalApplyOplogUpdate: {oplogUpdate: event.rawUpdateDescription}}]));
    }
    assertCollectionsAreIdentical(origColl, copyColl);
}

assert.commandWorked(db.t1Copy.insert(db.t1.find().toArray()));
assertCollectionsAreIdentical(db.t1, db.t1Copy);
assert.commandWorked(db.t2Copy.insert(db.t2.find().toArray()));
assertCollectionsAreIdentical(db.t2, db.t2Copy);

//
// Test op-style updates.
//
jsTestLog("Testing op-style update with $inc");
assert.commandWorked(db.t1.update({_id: 3}, {$inc: {b: 2}}));
expected = {
    documentKey: {_id: 3},
    ns: {db: "test", coll: "t1"},
    operationType: "update",
    rawUpdateDescription: {"$v": NumberInt(2), diff: {u: {b: 3}}}
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});
assertCanApplyRawUpdate(db.t1, db.t1Copy, expected);

jsTestLog("Testing op-style update with $set and multi:true");
assert.commandWorked(db.t1.update({a: 0}, {$set: {b: 2}}, {multi: true}));
expected = [
    {
        documentKey: {_id: 4},
        ns: {db: "test", coll: "t1"},
        operationType: "update",
        rawUpdateDescription: {"$v": NumberInt(2), diff: {u: {b: 2}}}
    },
    {
        documentKey: {_id: 5},
        ns: {db: "test", coll: "t1"},
        operationType: "update",
        rawUpdateDescription: {"$v": NumberInt(2), diff: {u: {b: 2}}}
    }
];
cst.assertNextChangesEqualUnordered({cursor: cursor, expectedChanges: expected});
assertCanApplyRawUpdate(db.t1, db.t1Copy, expected);

jsTestLog("Testing op-style update with $unset");
assert.commandWorked(db.t1.update({_id: 3}, {$unset: {b: ""}}));
expected = {
    documentKey: {_id: 3},
    ns: {db: "test", coll: "t1"},
    operationType: "update",
    rawUpdateDescription: {"$v": NumberInt(2), diff: {d: {b: false}}}
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});
assertCanApplyRawUpdate(db.t1, db.t1Copy, expected);

jsTestLog("Testing op-style update with $set on nested field");
assert.commandWorked(db.t1.update({_id: 8}, {$set: {"b.d": 2}}));
expected = {
    documentKey: {_id: 8},
    ns: {db: "test", coll: "t1"},
    operationType: "update",
    rawUpdateDescription: {"$v": NumberInt(2), diff: {sb: {i: {d: 2}}}}
};
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});
assertCanApplyRawUpdate(db.t1, db.t1Copy, expected);

//
// Test pipeline-style updates.
//
cursor = cst.startWatchingChanges(
    {pipeline: [{$changeStream: {showRawUpdateDescription: true}}], collection: db.t2});

// Also test a second change stream with the 'fullDocument' option enabled.
const fullDocCursor = cst.startWatchingChanges({
    pipeline: [{$changeStream: {showRawUpdateDescription: true, fullDocument: "updateLookup"}}],
    collection: db.t2
});

jsTestLog("Testing pipeline-style update with $set");
assert.commandWorked(db.t2.update({_id: 100}, [{
                                      $set: {
                                          a: 2,
                                          arrayForSubdiff: [kGiantStr, {a: kMediumStr, b: 3}],
                                          arrayForReplacement: [0],
                                          c: 3
                                      }
                                  }]));
expected = {
    documentKey: {_id: 100},
    fullDocument: {
        _id: 100,
        a: 2,
        b: 2,
        arrayForSubdiff: [kGiantStr, {a: kMediumStr, b: 3}],
        arrayForReplacement: [0],
        giantStr: kGiantStr,
        c: 3,
    },
    ns: {db: "test", coll: "t2"},
    operationType: "update",
    rawUpdateDescription: {
        "$v": NumberInt(2),
        diff: {
            u: {a: 2, arrayForReplacement: [0]},
            i: {c: 3},
            sarrayForSubdiff: {a: true, l: NumberInt(2), s1: {i: {b: 3}}}
        }
    }
};
cst.assertNextChangesEqual({cursor: fullDocCursor, expectedChanges: [expected]});
delete expected.fullDocument;
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});
assertCanApplyRawUpdate(db.t2, db.t2Copy, expected);

jsTestLog("Testing pipeline-style update with $unset");
assert.commandWorked(db.t2.update({_id: 100}, [{$unset: ["a"]}]));
expected = {
    documentKey: {_id: 100},
    fullDocument: {
        _id: 100,
        b: 2,
        arrayForSubdiff: [kGiantStr, {a: kMediumStr, b: 3}],
        arrayForReplacement: [0],
        giantStr: kGiantStr,
        c: 3
    },
    ns: {db: "test", coll: "t2"},
    operationType: "update",
    rawUpdateDescription: {"$v": NumberInt(2), diff: {d: {a: false}}}
};
cst.assertNextChangesEqual({cursor: fullDocCursor, expectedChanges: [expected]});
delete expected.fullDocument;
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});
assertCanApplyRawUpdate(db.t2, db.t2Copy, expected);

jsTestLog("Testing pipeline-style update with $replaceRoot");
assert.commandWorked(
    db.t2.update({_id: 100}, [{$replaceRoot: {newRoot: {_id: 100, "giantStr": kGiantStr}}}]));
expected = {
    documentKey: {_id: 100},
    fullDocument: {_id: 100, giantStr: kGiantStr},
    ns: {db: "test", coll: "t2"},
    operationType: "update",
    rawUpdateDescription: {
        "$v": NumberInt(2),
        diff: {d: {c: false, arrayForReplacement: false, arrayForSubdiff: false, b: false}}
    }
};
cst.assertNextChangesEqual({cursor: fullDocCursor, expectedChanges: [expected]});
delete expected.fullDocument;
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});
assertCanApplyRawUpdate(db.t2, db.t2Copy, expected);

jsTestLog("Testing pipeline-style update with a complex pipeline");
assert.commandWorked(db.t2.update({_id: 100}, [
    {
        $replaceRoot: {
            // Also constructing a new doc for later test.
            newRoot: {
                _id: 100,
                giantStr: kGiantStr,
                arr: [{x: 1, y: kSmallStr}, kMediumStr],
                arr_a: [1, kMediumStr],
                arr_b: [[1, kSmallStr], kMediumStr],
                arr_c: [[kSmallStr, 1, 2, 3], kMediumStr],
                obj: {x: {a: 1, b: 1, c: [kMediumStr, 1, 2, 3], str: kMediumStr}},
            }
        }
    },
    {$addFields: {a: "updated", b: 2, doc: {a: {0: "foo"}}}},
    {
        $project: {
            a: true,
            giantStr: true,
            doc: true,
            arr: true,
            arr_a: true,
            arr_b: true,
            arr_c: true,
            obj: true
        }
    },
]));

expected = {
    documentKey: {_id: 100},
    fullDocument: {
        _id: 100,
        giantStr: kGiantStr,
        arr: [{x: 1, y: kSmallStr}, kMediumStr],
        arr_a: [1, kMediumStr],
        arr_b: [[1, kSmallStr], kMediumStr],
        arr_c: [[kSmallStr, 1, 2, 3], kMediumStr],
        obj: {x: {a: 1, b: 1, c: [kMediumStr, 1, 2, 3], str: kMediumStr}},
        a: "updated",
        doc: {a: {0: "foo"}}
    },
    ns: {db: "test", coll: "t2"},
    operationType: "update",
    rawUpdateDescription: {
        "$v": NumberInt(2),
        diff: {
            i: {
                arr: [{x: 1, y: kSmallStr}, kMediumStr],
                arr_a: [1, kMediumStr],
                arr_b: [[1, kSmallStr], kMediumStr],
                arr_c: [[kSmallStr, 1, 2, 3], kMediumStr],
                obj: {x: {a: 1, b: 1, c: [kMediumStr, 1, 2, 3], str: kMediumStr}},
                a: "updated",
                doc: {a: {0: "foo"}}
            }
        }
    }
};
cst.assertNextChangesEqual({cursor: fullDocCursor, expectedChanges: [expected]});
delete expected.fullDocument;
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});
assertCanApplyRawUpdate(db.t2, db.t2Copy, expected);

jsTestLog("Testing pipeline-style update with modifications to nested elements");
assert.commandWorked(db.t2.update({_id: 100}, [{
                                      $replaceRoot: {
                                          newRoot: {
                                              _id: 100,
                                              giantStr: kGiantStr,
                                              arr: [{y: kSmallStr}, kMediumStr],
                                              arr_a: [2, kMediumStr],
                                              arr_b: [[2, kSmallStr], kMediumStr],
                                              arr_c: [[kSmallStr], kMediumStr],
                                              obj: {x: {b: 2, c: [kMediumStr], str: kMediumStr}},
                                          }
                                      }
                                  }]));
expected = {
    documentKey: {_id: 100},
    fullDocument: {
        _id: 100,
        giantStr: kGiantStr,
        arr: [{y: kSmallStr}, kMediumStr],
        arr_a: [2, kMediumStr],
        arr_b: [[2, kSmallStr], kMediumStr],
        arr_c: [[kSmallStr], kMediumStr],
        obj: {x: {b: 2, c: [kMediumStr], str: kMediumStr}},
    },
    ns: {db: "test", coll: "t2"},
    operationType: "update",
    rawUpdateDescription: {
        "$v": NumberInt(2),
        diff: {
            d: {a: false, doc: false},
            sarr: {a: true, s0: {d: {x: false}}},
            sarr_a: {a: true, u0: 2},
            sarr_b: {a: true, s0: {a: true, u0: 2}},
            sarr_c: {a: true, s0: {a: true, l: NumberInt(1)}},
            sobj: {sx: {d: {a: false}, u: {b: 2}, sc: {a: true, l: NumberInt(1)}}}
        }
    }
};
cst.assertNextChangesEqual({cursor: fullDocCursor, expectedChanges: [expected]});
delete expected.fullDocument;
cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});
assertCanApplyRawUpdate(db.t2, db.t2Copy, expected);

cst.cleanUp();
}());
