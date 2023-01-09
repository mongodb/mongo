/**
 * Tests pipeline-style updates that use the $_internalApplyOplogUpdate aggregate stage.
 * @tags: [
 *   requires_fcv_60,
 *   requires_multi_updates,
 *   requires_non_retryable_writes,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");       // For arrayEq.
load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

// Drop and recreate the collections to be used in this set of tests.
assertDropAndRecreateCollection(db, "t1");
assertDropAndRecreateCollection(db, "t2");

let documents1 = [
    {_id: 2, a: 4},
    {_id: 3, a: 5, b: 1},
    {_id: 4, a: 0, b: 1},
    {_id: 5, a: 0, b: 1},
    {_id: 8, a: 2, b: {c: 1}}
];

assert.commandWorked(db.t1.insert(documents1));

const kGiantStr = '*'.repeat(1024);
const kMediumStr = '*'.repeat(128);
const kSmallStr = '*'.repeat(32);

let documents2 = [{
    _id: 100,
    "a": 1,
    "b": 2,
    "arrayForSubdiff": [kGiantStr, {a: kMediumStr}, 1, 2, 3],
    "arrayForReplacement": [0, 1, 2, 3],
    "giantStr": kGiantStr
}];

assert.commandWorked(db.t2.insert(documents2));

//
// Test $_internalApplyOplogUpdate with v2 oplog update descriptions. For each update description,
// we execute $_internalApplyOplogUpdate twice to verify idempotency.
//

function testUpdate(expected, coll, filter, oplogUpdate, opts = {}) {
    for (let i = 0; i < 2; ++i) {
        assert.commandWorked(
            coll.update(filter, [{$_internalApplyOplogUpdate: {oplogUpdate: oplogUpdate}}], opts));

        let actual = coll.find().toArray();
        assert(arrayEq(expected, actual),
               () => "i: " + i + "  actual: " + tojson(actual) + "  expected: " + tojson(expected));
    }
}

let oplogUpdate = {"$v": NumberInt(2), diff: {u: {b: 3}}};
documents1[1].b = 3;
testUpdate(documents1, db.t1, {_id: 3}, oplogUpdate);

oplogUpdate = {
    "$v": NumberInt(2),
    diff: {d: {b: false}}
};

delete documents1[1].b;
testUpdate(documents1, db.t1, {_id: 3}, oplogUpdate);

// Test an update with upsert=true where no documents match the filter prior to the update.
oplogUpdate = {
    "$v": NumberInt(2),
    diff: {u: {b: 3}}
};
documents1.push({_id: 9, b: 3});
testUpdate(documents1, db.t1, {_id: 9}, oplogUpdate, {upsert: true});

oplogUpdate = {
    "$v": NumberInt(2),
    diff: {
        u: {a: 2, arrayForReplacement: [0]},
        i: {c: 3},
        sarrayForSubdiff: {a: true, l: NumberInt(2), s1: {i: {b: 3}}}
    }
};
documents2[0].a = 2;
documents2[0].arrayForSubdiff = [kGiantStr, {a: kMediumStr, b: 3}];
documents2[0].arrayForReplacement = [0];
documents2[0].c = 3;
testUpdate(documents2, db.t2, {_id: 100}, oplogUpdate);

oplogUpdate = {
    "$v": NumberInt(2),
    diff: {d: {a: false}}
};
delete documents2[0].a;
testUpdate(documents2, db.t2, {_id: 100}, oplogUpdate);

oplogUpdate = {
    "$v": NumberInt(2),
    diff: {d: {c: false, arrayForReplacement: false, arrayForSubdiff: false, b: false}}
};
documents2[0] = {
    _id: 100,
    "giantStr": kGiantStr
};
testUpdate(documents2, db.t2, {_id: 100}, oplogUpdate);

oplogUpdate = {
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
};
documents2[0] = {
    _id: 100,
    giantStr: kGiantStr,
    arr: [{x: 1, y: kSmallStr}, kMediumStr],
    arr_a: [1, kMediumStr],
    arr_b: [[1, kSmallStr], kMediumStr],
    arr_c: [[kSmallStr, 1, 2, 3], kMediumStr],
    obj: {x: {a: 1, b: 1, c: [kMediumStr, 1, 2, 3], str: kMediumStr}},
    a: "updated",
    doc: {a: {0: "foo"}}
};
testUpdate(documents2, db.t2, {_id: 100}, oplogUpdate);

oplogUpdate = {
    "$v": NumberInt(2),
    diff: {
        d: {a: false, doc: false},
        sarr: {a: true, s0: {d: {x: false}}},
        sarr_a: {a: true, u0: 2},
        sarr_b: {a: true, s0: {a: true, u0: 2}},
        sarr_c: {a: true, s0: {a: true, l: NumberInt(1)}},
        sobj: {sx: {d: {a: false}, u: {b: 2}, sc: {a: true, l: NumberInt(1)}}}
    }
};
documents2[0] = {
    _id: 100,
    giantStr: kGiantStr,
    arr: [{y: kSmallStr}, kMediumStr],
    arr_a: [2, kMediumStr],
    arr_b: [[2, kSmallStr], kMediumStr],
    arr_c: [[kSmallStr], kMediumStr],
    obj: {x: {b: 2, c: [kMediumStr], str: kMediumStr}},
};
testUpdate(documents2, db.t2, {_id: 100}, oplogUpdate);
}());
