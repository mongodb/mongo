(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode() and assertArrayEq().

const collName = "jstests_aggregation_add";
const coll = db["collName"];
coll.drop();

const x = "$x";  // fieldpath to "block" constant folding

/**
 * Verify constant folding with explain output.
 * @param {(number | number[])[]} input Input arithmetic parameters, optionally nested deeply.
 * @param {number[] | number} expectedOutput Expected output parameters after constant folding, or a
 *     scalar if the operation was calculated statically.
 * @param {string} message error message
 * @returns true if the explain output matches expectedOutput, and an assertion failure otherwise.
 */
function assertConstantFoldingResultForOp(op, input, expectedOutput, message) {
    const buildExpressionFromArguments = (arr, op) => {
        if (Array.isArray(arr)) {
            return {[op]: arr.map(elt => buildExpressionFromArguments(elt, op))};
        } else if (typeof arr === 'string' || arr instanceof String) {
            return arr;
        } else {
            return {$const: arr};
        }
    };
    const expected = buildExpressionFromArguments(expectedOutput, op);

    let pipeline = [
        {$group: {_id: buildExpressionFromArguments(input, op), sum: {$sum: 1}}},
    ];

    let result = db.runCommand({
        explain: {aggregate: collName, pipeline: pipeline, cursor: {}},
        verbosity: 'queryPlanner'
    });

    assert(result.stages && result.stages[1] && result.stages[1].$group, result);
    assert.eq(result.stages[1].$group._id, expected, message);

    // TODO: Verify that SBE does the right thing when project is pushed down.
    // pipeline = [{$project: {result: buildExpressionFromArguments(input, op)}}];
    // result = db.runCommand({
    //     explain: {aggregate: collName, pipeline: pipeline, cursor: {}},
    //     verbosity: 'queryPlanner'
    // });

    // assert(result.queryPlanner && result.queryPlanner.winningPlan &&
    //            result.queryPlanner.winningPlan.transformBy &&
    //            result.queryPlanner.winningPlan.transformBy.result,
    //        result);
    // assert.eq(result.queryPlanner.winningPlan.transformBy.result, expected, message);

    return true;
}

function assertConstantFoldingResults(input, addOutput, multiplyOutput, message) {
    assertConstantFoldingResultForOp("$add", input, addOutput, message);
    assertConstantFoldingResultForOp("$multiply", input, multiplyOutput, message);
}

// Totally fold constants.
assertConstantFoldingResults([1, 2, 3], 6, 6, "All constants should fold.");
assertConstantFoldingResults(
    [[1, 2], 3, 4, 5], 15, 120, "Nested operations with all constants should be folded away.");

// Left-associative test cases.
assertConstantFoldingResults([1, 2, x],
                             [3, x],
                             [2, x],
                             "Constants should fold left-to-right before the first non-constant.");
assertConstantFoldingResults(
    [x, 1, 2],
    [x, 1, 2],
    [x, 1, 2],
    "Constants should not fold left-to-right after the first non-constant.");
assertConstantFoldingResults(
    [1, x, 2], [1, x, 2], [1, x, 2], "Constants should not fold across non-constants.");

assertConstantFoldingResults(
    [5, 2, x, 3, 4], [7, x, 3, 4], [10, x, 3, 4], "Constants should fold up until a non-constant.");

assertConstantFoldingResults([x, 1, 2, 3],
                             [x, 1, 2, 3],
                             [x, 1, 2, 3],
                             "Non-constant at start of operand list blocks folding constants.");

// Non-optimized comparisons -- make sure that non-optimized pipelines will give the same result as
// optimized ones.
// This is a regression test for BF-24149.
coll.insert({_id: 0, v: NumberDecimal("917.6875119062092")});
coll.insert({_id: 1, v: NumberDecimal("927.3345924210555")});

const pipeline = [
    {$group: {_id: {$multiply: [-3.14159265859, "$v", -314159255]}, sum: {$sum: 1}}},
];
const result = coll.aggregate(pipeline).toArray();
assertArrayEq({
    actual: result,
    expected: [
        {"_id": NumberDecimal("915242528741.9469524422272990976000"), "sum": 1},
        {"_id": NumberDecimal("905721242210.0453137831269007622941"), "sum": 1}
    ]
});

// Function to generate random numbers of float, long, double, and NumberDecimal (with different
// probabilities).
const randomNumber = (min, max) => {
    const r = Math.random() * (max - min) + min;
    const t = Math.random();
    if (t < 0.7) {
        return r;
    }
    if (t < 0.9) {
        return NumberInt(Math.round(r));
    }
    if (t < 0.999) {
        return NumberLong(Math.round(r));
    }
    return NumberDecimal(String(r));
}
})();