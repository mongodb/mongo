// $group has inconsistent behavior when differentiating between null and missing values, provided
// this test passes. Here, we check the cases where it is correct, and those where it is currently
// incorrect.
//
// This test issues some pipelines where it assumes an initial $sort will be absorbed and be
// covered, which will not happen if the $sort is within a $facet stage.
// @tags: [
//   do_not_wrap_aggregations_in_facets,
// ]
load('jstests/aggregation/extras/utils.js');  // For assertArrayEq.

(function() {
"use strict";

const coll = db.getCollection(jsTestName());
coll.drop();

assert.commandWorked(coll.insert({a: null}));
assert.commandWorked(coll.insert({}));

let res = coll.aggregate({$group: {_id: "$a"}});
let arr = res.toArray();
assert.eq(arr.length, 1);
assert.eq(arr[0]._id, null);

assert.commandWorked(coll.createIndex({a: 1}));
res = coll.aggregate({$sort: {a: 1}}, {$group: {_id: "$a"}});
arr = res.toArray();
assert.eq(arr.length, 1);
assert.eq(arr[0]._id, null);

coll.drop();

assert.commandWorked(coll.insert({a: null}));
assert.commandWorked(coll.insert({}));

// Bug, see SERVER-21992.
res = coll.aggregate({$group: {_id: {a: "$a"}}});
assertArrayEq({actual: res.toArray(), expected: [{_id: {a: null}}]});

// Bug, see SERVER-21992.
assert.commandWorked(coll.createIndex({a: 1}));
res = coll.aggregate({$group: {_id: {a: "$a"}}});
assertArrayEq({actual: res.toArray(), expected: [{_id: {a: null}}]});

// Correct behavior after SERVER-21992 is fixed.
if (0) {
    res = coll.aggregate({$group: {_id: {a: "$a"}}});
    assertArrayEq({actual: res.toArray(), expected: [{_id: {a: null}}, {_id: {}}]});
}

coll.drop();
assert.commandWorked(coll.insert({a: null, b: 1}));
assert.commandWorked(coll.insert({b: 1}));
assert.commandWorked(coll.insert({a: null, b: 1}));

res = coll.aggregate({$group: {_id: {a: "$a", b: "$b"}}});
assertArrayEq({actual: res.toArray(), expected: [{_id: {b: 1}}, {_id: {a: null, b: 1}}]});

assert.commandWorked(coll.createIndex({a: 1, b: 1}));
res = coll.aggregate([{$group: {_id: {a: "$a", b: "$b"}}}, {$sort: {"_id.a": 1, "_id.b": 1}}]);
// Before fixing SERVER-23229 we were getting [{_id: {a: null, b: 1}}]
assertArrayEq({actual: res.toArray(), expected: [{_id: {b: 1}}, {_id: {a: null, b: 1}}]});

// Try another variation of the query that is taken more directly from the bug report SERVER-23229.
coll.drop();
assert.commandWorked(coll.insert({a: 1, b: null}));
assert.commandWorked(coll.insert({a: null, b: 1}));
assert.commandWorked(coll.insert({b: 1}));
assert.commandWorked(coll.insert({a: 1}));

let preSortResult =
    coll.aggregate({$sort: {a: 1, b: 1}}, {$group: {_id: {a: "$a", b: "$b"}}}).toArray();
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assertArrayEq({
    actual: preSortResult,
    expected: coll.aggregate({$group: {_id: {a: "$a", b: "$b"}}}).toArray()
});
}());
