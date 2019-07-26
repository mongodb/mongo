/**
 * Test $** support for the $type operator.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getPlanStages.

const coll = db.wildcard_index_type;
coll.drop();

const indexWildcard = {
    "$**": 1
};

// Inserts the given document and runs the given query to confirm that:
// (1) query matches the given document if match is true,
// (2) the winning plan does a wildcard index scan, and
// (3) the resulting index bound matches 'expectedBounds' if given.
function assertExpectedDocAnswersWildcardIndexQuery(doc, query, match, expectedBounds) {
    coll.drop();
    assert.commandWorked(coll.createIndex(indexWildcard));
    assert.commandWorked(coll.insert(doc));

    // Check that a wildcard index scan is being used to answer query.
    const explain = coll.explain("executionStats").find(query).finish();
    if (!match) {
        assert.eq(0, explain.executionStats.nReturned, explain);
        return;
    }

    // Check that the query returns the document.
    assert.eq(1, explain.executionStats.nReturned, explain);

    // Winning plan uses a wildcard index scan.
    const winningPlan = explain.queryPlanner.winningPlan;
    const ixScans = getPlanStages(winningPlan, "IXSCAN");
    assert.gt(ixScans.length, 0, explain);
    ixScans.forEach((ixScan) => assert(ixScan.keyPattern.$_path));

    // Expected bounds were used.
    if (expectedBounds !== undefined) {
        ixScans.forEach((ixScan) => assert.docEq(ixScan.indexBounds, expectedBounds));
    }
}

// A $type of 'string' will match a string value.
assertExpectedDocAnswersWildcardIndexQuery({a: "a"}, {a: {$type: "string"}}, true);

// A $type of 'double' will match a double.
assertExpectedDocAnswersWildcardIndexQuery({a: 1.1}, {a: {$type: "double"}}, true);

// A $type of 'boolean' will match a boolean.
assertExpectedDocAnswersWildcardIndexQuery({a: true}, {a: {$type: "bool"}}, true);

// A $type of 'string' will match a multifield document with a string value.
assertExpectedDocAnswersWildcardIndexQuery({a: "a", b: 1.1, c: true}, {a: {$type: "string"}}, true);

// A compound $type of 'string' and 'double' will match a multifield document with a string and
// double value.
assertExpectedDocAnswersWildcardIndexQuery(
    {a: "a", b: 1.1, c: true}, {a: {$type: "string"}, b: {$type: "double"}}, true);

// A compound $type of 'string' and 'double' won't match a multifield document with a string but
// no double value.
assertExpectedDocAnswersWildcardIndexQuery(
    {a: "a", b: "b", c: true}, {a: {$type: "string"}, b: {$type: "double"}}, false);

// A $type of 'object' will match a object.
assertExpectedDocAnswersWildcardIndexQuery(
    {a: {"": ""}},
    {a: {$type: "object"}},
    true,
    {$_path: [`["a", "a"]`, `["a.", "a/")`], a: [`[MinKey, MaxKey]`]});

// A $type of 'object' will match an empty object.
assertExpectedDocAnswersWildcardIndexQuery(
    {a: {}},
    {a: {$type: "object"}},
    true,
    {$_path: [`["a", "a"]`, `["a.", "a/")`], a: [`[MinKey, MaxKey]`]});

// A $type of 'object' will match a nested object.
assertExpectedDocAnswersWildcardIndexQuery(
    {b: {a: {}}},
    {"b.a": {$type: "object"}},
    true,
    {$_path: [`["b.a", "b.a"]`, `["b.a.", "b.a/")`], "b.a": [`[MinKey, MaxKey]`]});

// A $type of 'array' will match an empty array.
assertExpectedDocAnswersWildcardIndexQuery(
    {a: [[]]},
    {a: {$type: "array"}},
    true,
    {$_path: [`["a", "a"]`, `["a.", "a/")`], a: [`[MinKey, MaxKey]`]});

// A $type of 'array' will match an array.
assertExpectedDocAnswersWildcardIndexQuery(
    {a: [["c"]]},
    {a: {$type: "array"}},
    true,
    {$_path: [`["a", "a"]`, `["a.", "a/")`], a: [`[MinKey, MaxKey]`]});

// A $type of 'regex' will match regex.
assertExpectedDocAnswersWildcardIndexQuery({a: /r/}, {a: {$type: "regex"}}, true);

// A $type of 'null' will match a null value.
assertExpectedDocAnswersWildcardIndexQuery({a: null}, {a: {$type: "null"}}, true);

// A $type of 'undefined' will match undefined.
assertExpectedDocAnswersWildcardIndexQuery({a: undefined}, {a: {$type: "undefined"}}, true);

// A $type of 'undefined' won't match a null value.
assertExpectedDocAnswersWildcardIndexQuery({a: null}, {a: {$type: "undefined"}}, false);

// A $type of 'code' will match a function value.
assertExpectedDocAnswersWildcardIndexQuery({
    a: function() {
        var a = 0;
    }
},
                                           {a: {$type: "javascript"}},
                                           true);

// A $type of 'binData' will match a binData value.
assertExpectedDocAnswersWildcardIndexQuery({a: new BinData(0, "")}, {a: {$type: "binData"}}, true);

// A $type of 'timestamp' will match an empty timestamp value.
assertExpectedDocAnswersWildcardIndexQuery({a: new Timestamp()}, {a: {$type: "timestamp"}}, true);

// A $type of 'timestamp' will match a timestamp value.
assertExpectedDocAnswersWildcardIndexQuery(
    {a: new Timestamp(0x80008000, 0)}, {a: {$type: "timestamp"}}, true);

// A $type of 'date' won't match a timestamp value.
assertExpectedDocAnswersWildcardIndexQuery(
    {a: new Timestamp(0x80008000, 0)}, {a: {$type: "date"}}, false);

// A $type of 'date' will match a date value.
assertExpectedDocAnswersWildcardIndexQuery({a: new Date()}, {a: {$type: "date"}}, true);

// A $type of 'timestamp' won't match a date value.
assertExpectedDocAnswersWildcardIndexQuery({a: new Date()}, {a: {$type: "timestamp"}}, false);
})();
