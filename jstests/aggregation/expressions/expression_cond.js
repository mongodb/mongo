// Confirm correctness of $cond expression evaluation.
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');

const coll = db.expressionCond;

coll.drop();
assert.commandWorked(
    coll.insert([{_id: 0, year: 1999}, {_id: 1, year: 2021}, {_id: 2, year: 2000}]));

function runAndAssert(condSpec, expectedResult) {
    assertArrayEq({
        expected: expectedResult,
        actual: coll.aggregate([{$project: {year: 1, century: {$cond: condSpec}}}]).toArray()
    });
}

function runAndAssertThrows(condSpec, expectedErrorCode) {
    assertErrorCode(coll, {$project: {foo: {$cond: condSpec}}}, expectedErrorCode);
}

// Tests non constant condition case.
runAndAssert([{$gt: ["$year", 2000]}, "new", "old"], [
    {_id: 0, year: 1999, century: "old"},
    {_id: 1, year: 2021, century: "new"},
    {_id: 2, year: 2000, century: "old"}
]);

// Tests constant true condition case.
runAndAssert([true, {$concat: ["n", "e", "w"]}, "old"], [
    {_id: 0, year: 1999, century: "new"},
    {_id: 1, year: 2021, century: "new"},
    {_id: 2, year: 2000, century: "new"}
]);

// Tests constant false condition case.
runAndAssert([false, "new", {$concat: ["o", "l", "d"]}], [
    {_id: 0, year: 1999, century: "old"},
    {_id: 1, year: 2021, century: "old"},
    {_id: 2, year: 2000, century: "old"}
]);

// Tests a case when condition arguments is a constant expression.
runAndAssert([{$eq: ["A", "a"]}, "new", {$concat: ["o", "l", "d"]}], [
    {_id: 0, year: 1999, century: "old"},
    {_id: 1, year: 2021, century: "old"},
    {_id: 2, year: 2000, century: "old"}
]);

// Tests a case when condition arguments is null.
runAndAssert([null, "new", "old"], [
    {_id: 0, year: 1999, century: "old"},
    {_id: 1, year: 2021, century: "old"},
    {_id: 2, year: 2000, century: "old"}
]);

// Tests a case when condition arguments is undefined.
runAndAssert([undefined, "new", "old"], [
    {_id: 0, year: 1999, century: "old"},
    {_id: 1, year: 2021, century: "old"},
    {_id: 2, year: 2000, century: "old"}
]);

// Tests a case when then-arguments is undefined.
runAndAssert([true, undefined, "old"], [
    {_id: 0, year: 1999, century: undefined},
    {_id: 1, year: 2021, century: undefined},
    {_id: 2, year: 2000, century: undefined}
]);

// Tests a case when else-arguments is null.
runAndAssert([undefined, null, null], [
    {_id: 0, year: 1999, century: null},
    {_id: 1, year: 2021, century: null},
    {_id: 2, year: 2000, century: null}
]);

// Tests a case with incorrect arguments.
runAndAssertThrows(["new", "old"], 16020);
runAndAssertThrows([], 16020);
runAndAssertThrows(null, 16020);
runAndAssertThrows(undefined, 16020);

// Tests a normal case using extended syntax.
runAndAssert(
    {
        if: {$gt: ["$year", 2000]}, then: "new", else: "old"
    },
    [
        {_id: 0, year: 1999, century: "old"},
        {_id: 1, year: 2021, century: "new"},
        {_id: 2, year: 2000, century: "old"}
    ]);

// Test a case using extended syntax with invalid parameters.
runAndAssertThrows({i: {$gt: ["$year", 2000]}, then: "new", else: "old"}, 17083);
runAndAssertThrows({then: "new", else: "old"}, 17080);
runAndAssertThrows({
    if: {$gt: ["$year", 2000]}, else: "old"
}, 17081);
runAndAssertThrows({
    if: {$gt: ["$year", 2000]}, then: "new"
}, 17082);

// Tests nested $cond expressions.
runAndAssert(
    {
        if: {$gt: ["$year", 2000]},
        then: "new",
        else: {$cond: [{$eq: ["$year", 2000]}, "2000 was in XX century", "old"]}
    },
    [
        {_id: 0, year: 1999, century: "old"},
        {_id: 1, year: 2021, century: "new"},
        {_id: 2, year: 2000, century: "2000 was in XX century"}
    ]);
})();
