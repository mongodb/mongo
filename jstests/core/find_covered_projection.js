/**
 * Tests queries that are covered by an index.
 *
 * This test cannot implicitly shard accessed collections because queries on a sharded collection
 * cannot be covered when they aren't on the shard key since the document needs to be fetched in
 * order to apply the SHARDING_FILTER stage.
 * @tags: [
 *   assumes_unsharded_collection,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // arrayEq

const coll = db.jstests_find_covered_projection;
coll.drop();

assert.commandWorked(coll.insert([
    {order: 0, fn: "john", ln: "doe"},
    {order: 1, fn: "jack", ln: "doe"},
    {order: 2, fn: "john", ln: "smith"},
    {order: 3, fn: "jack", ln: "black"},
    {order: 4, fn: "bob", ln: "murray"},
    {order: 5, fn: "aaa", ln: "bbb", obj: {a: 1, b: "blah"}}
]));

function assertResultsMatch(query, projection, expectedResult) {
    assert(arrayEq(coll.find(query, projection).toArray(), expectedResult));
}

// Create an index on one field.
assert.commandWorked(coll.createIndex({ln: 1}));

assertResultsMatch({ln: "doe"}, {ln: 1, _id: 0}, [{ln: "doe"}, {ln: "doe"}]);

// Create a compound index.
assert.commandWorked(coll.dropIndex({ln: 1}));
assert.commandWorked(coll.createIndex({ln: 1, fn: 1}));

assertResultsMatch({ln: "doe"}, {ln: 1, _id: 0}, [{ln: "doe"}, {ln: "doe"}]);
assertResultsMatch(
    {ln: "doe"}, {ln: 1, fn: 1, _id: 0}, [{ln: "doe", fn: "jack"}, {ln: "doe", fn: "john"}]);
assertResultsMatch({ln: "doe", fn: "john"}, {ln: 1, fn: 1, _id: 0}, [{ln: "doe", fn: "john"}]);
assertResultsMatch({fn: "john", ln: "doe"}, {fn: 1, ln: 1, _id: 0}, [{ln: "doe", fn: "john"}]);

// Repeat the above test, but with a compound index involving _id.
assert.commandWorked(coll.dropIndex({ln: 1, fn: 1}));
assert.commandWorked(coll.createIndex({_id: 1, ln: 1}));

assertResultsMatch({_id: 123, ln: "doe"}, {_id: 1}, []);
assertResultsMatch({_id: 123, ln: "doe"}, {ln: 1}, []);
assertResultsMatch({ln: "doe", _id: 123}, {ln: 1, _id: 1}, []);

// Create an index on an embedded object.
assert.commandWorked(coll.dropIndex({_id: 1, ln: 1}));
assert.commandWorked(coll.createIndex({obj: 1}));

assertResultsMatch({obj: {a: 1, b: "blah"}}, {obj: 1, _id: 0}, [{obj: {a: 1, b: "blah"}}]);
}());
