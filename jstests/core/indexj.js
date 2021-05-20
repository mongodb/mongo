// SERVER-726
// This test makes assertions about how many keys are examined during query execution, which can
// change depending on whether/how many documents are filtered out by the SHARDING_FILTER stage.
// @tags: [
//   assumes_unsharded_collection,
//   # This test makes assertions about the types of plans produced by the query engine, which has
//   # changed from the classic engine starting in version 5.0.
//   requires_fcv_50,
// ]

(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const t = db[jsTestName()];
t.drop();

const isSBEEnabled = checkSBEEnabled(db);

function keysExamined(query, hint, sort) {
    if (!hint) {
        hint = {};
    }
    if (!sort) {
        sort = {};
    }
    const explain = t.find(query).sort(sort).hint(hint).explain("executionStats");
    return explain.executionStats.totalKeysExamined;
}

assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.insert({a: 5}));
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}), "A");

assert(t.drop());
assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.insert({a: 4}));
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}), "B");

assert.commandWorked(t.insert({a: 5}));
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}), "D");

assert.commandWorked(t.insert({a: 4}));
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}), "C");

assert.commandWorked(t.insert({a: 5}));
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}), "D");

assert(t.drop());
assert.commandWorked(t.createIndex({a: 1, b: 1}));
assert.commandWorked(t.insert({a: 1, b: 1}));
assert.commandWorked(t.insert({a: 1, b: 2}));
assert.commandWorked(t.insert({a: 2, b: 1}));
assert.commandWorked(t.insert({a: 2, b: 2}));

// We make different assertions about the number of keys examined depending on whether we are using
// SBE or the classic engine. This is because the classic engine will use a multi-interval index
// scan whereas SBE will decompose the intervals into a set of single-interval bounds and will end
// up examining 0 keys.
let expectedKeys = isSBEEnabled ? 0 : 3;
assert.eq(expectedKeys, keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}));
assert.eq(expectedKeys,
          keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}, {a: -1, b: -1}));

assert.commandWorked(t.insert({a: 1, b: 1}));
assert.commandWorked(t.insert({a: 1, b: 1}));
assert.eq(expectedKeys, keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}));
assert.eq(expectedKeys, keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}));
assert.eq(expectedKeys,
          keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}, {a: -1, b: -1}));

// We examine one less key in the classic engine because the bounds are slightly tighter.
if (!isSBEEnabled) {
    expectedKeys = 2;
}
assert.eq(expectedKeys, keysExamined({a: {$in: [1, 1.9]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}));
assert.eq(expectedKeys,
          keysExamined({a: {$in: [1.1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}, {a: -1, b: -1}));

assert.commandWorked(t.insert({a: 1, b: 1.5}));

// We examine one extra key in both engines because we've inserted a document that falls within
// both sets of bounds being scanned.
expectedKeys = isSBEEnabled ? 1 : 4;
assert.eq(expectedKeys, keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}), "F");
})();
