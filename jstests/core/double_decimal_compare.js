/**
 * Verifies correctness of double/decimal comparisons depending on the engine being used. This
 * is intended to reproduce SERVER-58155.
 * @tags: [
 *   assumes_read_concern_local,
 *   no_selinux,
 * ]
 */
(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");  // For 'arrayEq'.

const docs = [{a: 5.01}, {a: NumberDecimal("5.01")}, {a: NumberDecimal("5.0100")}];
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert(docs));
const doubleQueryResults = coll.find({a: 5.01}, {_id: 0}).toArray();
const decimalQueryResults = coll.find({a: NumberDecimal("5.01")}, {_id: 0}).toArray();

// The double query will only match the single double value, and the decimal query will only match
// the decimal values.
assert.eq(doubleQueryResults, [{a: 5.01}], doubleQueryResults);
assert(arrayEq(decimalQueryResults, [{a: NumberDecimal("5.01")}, {a: NumberDecimal("5.0100")}]),
       decimalQueryResults);

assert.commandWorked(coll.createIndex({a: 1}));
const doubleQueryIndexResults = coll.find({a: 5.01}, {_id: 0}).toArray();
const decimalQueryIndexResults = coll.find({a: NumberDecimal("5.01")}, {_id: 0}).toArray();

// The double query will only match the single double value, and the decimal query will only match
// the decimal values.
assert.eq(doubleQueryIndexResults, [{a: 5.01}], doubleQueryIndexResults);
assert(
    arrayEq(decimalQueryIndexResults, [{a: NumberDecimal("5.01")}, {a: NumberDecimal("5.0100")}]),
    decimalQueryIndexResults);
}());
