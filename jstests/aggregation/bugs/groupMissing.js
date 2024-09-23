// $group has inconsistent behavior when differentiating between null and missing values, provided
// this test passes. Here, we check the cases where it is correct, and those where it is currently
// incorrect.
//
// This test issues some pipelines where it assumes an initial $sort will be absorbed and be
// covered, which will not happen if the $sort is within a $facet stage.
// @tags: [
//   do_not_wrap_aggregations_in_facets,
// ]
import {arrayEq, assertArrayEq} from "jstests/aggregation/extras/utils.js";

var coll = db.getCollection(jsTestName());
coll.drop();

coll.insert({a: null});
coll.insert({});

var res = coll.aggregate({$group: {_id: "$a"}});
var arr = res.toArray();
assert.eq(arr.length, 1);
assert.eq(arr[0]._id, null);

coll.createIndex({a: 1});
res = coll.aggregate({$sort: {a: 1}}, {$group: {_id: "$a"}});
arr = res.toArray();
assert.eq(arr.length, 1);
assert.eq(arr[0]._id, null);

coll.drop();

coll.insert({a: null});
coll.insert({});

// Bug, see SERVER-21992.
res = coll.aggregate({$group: {_id: {a: "$a"}}});
assertArrayEq({actual: res.toArray(), expected: [{_id: {a: null}}]});

// Bug, see SERVER-21992.
coll.createIndex({a: 1});
res = coll.aggregate({$group: {_id: {a: "$a"}}});
assertArrayEq({actual: res.toArray(), expected: [{_id: {a: null}}]});

// TODO(SERVER-21992): Re-enable the following tests
// res = coll.aggregate({$group: {_id: {a: "$a"}}});
// assertArrayEq({actual: res.toArray(), expected: [{_id: {a: null}}, {_id: {}}]});

coll.drop();
coll.insert({a: null, b: 1});
coll.insert({b: 1});
coll.insert({a: null, b: 1});

res = coll.aggregate({$group: {_id: {a: "$a", b: "$b"}}});
assertArrayEq({actual: res.toArray(), expected: [{_id: {b: 1}}, {_id: {a: null, b: 1}}]});

res = coll.aggregate({$sort: {a: 1, b: 1}}, {$group: {_id: {a: "$a", b: "$b"}}});
assertArrayEq({actual: res.toArray(), expected: [{_id: {b: 1}}, {_id: {a: null, b: 1}}]});

// Bug, see SERVER-23229.  Note that the presence of a sort w/index leads to a PROJECTION_COVERED.
coll.createIndex({a: 1, b: 1});
res = coll.aggregate({$sort: {a: 1, b: 1}}, {$group: {_id: {a: "$a", b: "$b"}}});
assertArrayEq({actual: res.toArray(), expected: [{_id: {a: null, b: 1}}]});

// TODO(SERVER-23229): Re-enable the following tests
// coll.createIndex({a: 1, b: 1});
// res = coll.aggregate({$sort: {a: 1, b: 1}}, {$group: {_id: {a: "$a", b: "$b"}}});
// assertArrayEq({actual: res.toArray(), expected: [{_id: {b: 1}}, {_id: {a: null, b: 1}}]});

// Try a simpler variation of the bug SERVER-23229, without $group.
coll.drop();
coll.insert({a: 1, b: null});
coll.insert({a: null, b: 1});
coll.insert({b: 1});
coll.insert({a: 1});

let collScanResult = coll.aggregate({$match: {a: 1}}, {$project: {_id: 0, a: 1, b: 1}}).toArray();
assertArrayEq({actual: collScanResult, expected: [{"a": 1, "b": null}, {"a": 1}]});

// After creating the index, the classic plan will use PROJECTION_COVERED, and the index will
// incorrectly provide a null for the missing "b" value.
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
const possibleResults = [collScanResult, [{"a": 1, "b": null}, {"a": 1, "b": null}]];

function checkActualMatchesAnExpected(actual) {
    let foundMatch = false;
    for (let i = 0; i < possibleResults.length; i++) {
        foundMatch |= arrayEq(actual, possibleResults[i]);
    }
    assert(foundMatch,
           `Expected actual results to match one of the possible results. actual=${
               tojson(actual)}, possibleResults=${tojson(possibleResults)}`);
}

// Check behavior with and without a hint.
checkActualMatchesAnExpected(
    coll.aggregate([{$match: {a: 1}}, {$project: {_id: 0, a: 1, b: 1}}]).toArray());
checkActualMatchesAnExpected(
    coll.aggregate([{$match: {a: 1}}, {$project: {_id: 0, a: 1, b: 1}}], {hint: {a: 1, b: 1}})
        .toArray());
