/**
 * Tests that $** indexes obey hinting.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq.
load("jstests/libs/analyze_plan.js");         // For getPlanStages.

const coll = db.wildcard_hint;
coll.drop();

const assertArrayEq = (l, r) => assert(arrayEq(l, r), tojson(l) + " != " + tojson(r));

// Extracts the winning plan for the given query and hint from the explain output.
const winningPlan = (query, hint) =>
    assert.commandWorked(coll.find(query).hint(hint).explain()).queryPlanner.winningPlan;

// Runs the given query and confirms that:
// (1) the expected index was used to answer the query, and
// (2) the results produced by the index match the given 'expectedResults'.
function assertExpectedIndexAnswersQueryWithHint(query, hint, expectedIndexName, expectedResults) {
    const ixScans = getPlanStages(winningPlan(query, hint), "IXSCAN");
    assert.gt(ixScans.length, 0, tojson(coll.find(query).hint(hint).explain()));
    ixScans.forEach((ixScan) => assert.eq(ixScan.indexName, expectedIndexName));

    const wildcardResults = coll.find(query, {_id: 0}).hint(hint).toArray();
    assertArrayEq(wildcardResults, expectedResults);
}

assert.commandWorked(db.createCollection(coll.getName()));

// Check that error is thrown if the hinted index doesn't exist.
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), filter: {"a": 1}, hint: {"$**": 1}}), ErrorCodes.BadValue);

assert.commandWorked(coll.createIndex({"$**": 1}));

assert.commandWorked(coll.insert({_id: 10, a: 1, b: 1, c: {d: 1, e: 1}}));
assert.commandWorked(coll.insert({a: 1, b: 2, c: {d: 2, e: 2}}));
assert.commandWorked(coll.insert({a: 2, b: 2, c: {d: 1, e: 2}}));
assert.commandWorked(coll.insert({a: 2, b: 1, c: {d: 2, e: 2}}));
assert.commandWorked(coll.insert({a: 2, b: 2, c: {e: 2}}));

// Hint a $** index without a competing index.
assertExpectedIndexAnswersQueryWithHint(
    {"a": 1}, {"$**": 1}, "$**_1", [{a: 1, b: 1, c: {d: 1, e: 1}}, {a: 1, b: 2, c: {d: 2, e: 2}}]);

assert.commandWorked(coll.createIndex({"a": 1}));

// Hint a $** index with a competing index.
assertExpectedIndexAnswersQueryWithHint(
    {"a": 1}, {"$**": 1}, "$**_1", [{a: 1, b: 1, c: {d: 1, e: 1}}, {a: 1, b: 2, c: {d: 2, e: 2}}]);

// Hint a $** index with a competing _id index.
assertExpectedIndexAnswersQueryWithHint(
    {"a": 1, "_id": 10}, {"$**": 1}, "$**_1", [{a: 1, b: 1, c: {d: 1, e: 1}}]);

// Hint a regular index with a competing $** index.
assertExpectedIndexAnswersQueryWithHint(
    {"a": 1}, {"a": 1}, "a_1", [{a: 1, b: 1, c: {d: 1, e: 1}}, {a: 1, b: 2, c: {d: 2, e: 2}}]);

// Query on fields that not all documents in the collection have with $** index hint.
assertExpectedIndexAnswersQueryWithHint(
    {"c.d": 1},
    {"$**": 1},
    "$**_1",
    [{a: 1, b: 1, c: {d: 1, e: 1}}, {a: 2, b: 2, c: {d: 1, e: 2}}]);

// Adding another wildcard index with a path specified.
assert.commandWorked(coll.createIndex({"c.$**": 1}));

// Hint on path that is not in query argument.
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), filter: {"a": 1}, hint: {"c.$**": 1}}),
    ErrorCodes.BadValue);

// Hint on a path specified $** index.
assertExpectedIndexAnswersQueryWithHint(
    {"c.d": 1},
    {"c.$**": 1},
    "c.$**_1",
    [{a: 2, b: 2, c: {d: 1, e: 2}}, {a: 1, b: 1, c: {d: 1, e: 1}}]);

// Min/max with $** index hint.
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), filter: {"b": 1}, min: {"a": 1}, hint: {"$**": 1}}),
    51174);

// Hint a $** index on a query with compound fields.
assertExpectedIndexAnswersQueryWithHint(
    {"a": 1, "c.e": 1}, {"$**": 1}, "$**_1", [{a: 1, b: 1, c: {d: 1, e: 1}}]);

// Hint a $** index by name.
assertExpectedIndexAnswersQueryWithHint(
    {"a": 1}, "$**_1", "$**_1", [{a: 1, b: 1, c: {d: 1, e: 1}}, {a: 1, b: 2, c: {d: 2, e: 2}}]);
})();
