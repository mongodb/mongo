// Verify that bitwise agg expressions work as expected.
// @tags: [
//   requires_fcv_63,
//   featureFlagBitwiseAggOperators,
// ]
load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
(function() {
"use strict";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, a: NumberInt(0), b: NumberInt(127), c: [NumberInt(0), NumberInt(127)]},
    {_id: 1, a: NumberInt(1), b: NumberInt(2), c: [NumberInt(1), NumberInt(2)]},
    {_id: 2, a: NumberInt(2), b: NumberInt(3), c: [NumberInt(2), NumberInt(3)]},
    {_id: 3, a: NumberInt(3), b: NumberInt(5), c: [NumberInt(3), NumberInt(5)]},
]));

function runAndAssert(expression, expectedResult) {
    assertArrayEq({
        actual: coll.aggregate([{$project: {r: {[expression]: ["$a", "$b"]}}}])
                    .toArray()
                    .map(doc => doc.r),
        expected: expectedResult
    });
}

runAndAssert("$bitAnd", [0, 0, 2, 1]);
runAndAssert("$bitOr", [127, 3, 3, 7]);
runAndAssert("$bitXor", [127, 3, 1, 6]);

for (const operator of ["$bitAnd", "$bitOr", "$bitXor"]) {
    for (const operand
             of [Number(12.0), NumberDecimal("12"), "$c", ["$c"], [[NumberInt(1), NumberInt(2)]]]) {
        assert.commandFailedWithCode(coll.runCommand({
            aggregate: collName,
            cursor: {},
            pipeline: [{
                $project: {
                    r: {[operator]: ["$a", operand]},
                }
            }]
        }),
                                     ErrorCodes.TypeMismatch);
    }
    for (const argument of ["$c", ["$c"], [[NumberInt(1), NumberInt(2)]]]) {
        assert.commandFailedWithCode(coll.runCommand({
            aggregate: collName,
            cursor: {},
            pipeline: [{
                $project: {
                    r: {[operator]: argument},
                }
            }]
        }),
                                     ErrorCodes.TypeMismatch);
    }
}

assertArrayEq({
    actual: coll.aggregate([
                    {$project: {r: {$bitNot: "$a"}}},
                ])
                .toArray()
                .map(doc => doc.r),
    expected: [-1, -2, -3, -4]
});

assert.commandFailedWithCode(coll.runCommand({
    aggregate: collName,
    cursor: {},
    pipeline: [{
        $project: {
            r: {$bitNot: 12.5},
        }
    }]
}),
                             ErrorCodes.TypeMismatch);

assert.commandFailedWithCode(coll.runCommand({
    aggregate: collName,
    cursor: {},
    pipeline: [{
        $project: {
            r: {$bitNot: ["$a", "$b"]},
        }
    }]
}),
                             16020);  // Error for incorrect number of arguments.
}());
