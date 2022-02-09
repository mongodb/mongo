(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode().

const coll = db.jstests_aggregation_add;
coll.drop();

// TODO: SERVER-63099 Make sure that constant folding does not assume associativity or
// commutativity, and that conversions to NumberDecimal are not different with/without optimization.

const x = "$x"  // fieldpath to "block" constant folding

/**
 * Verify constant folding with explain output.
 * @param {(number | number[])[]} input Input arithmetic parameters, optionally nested deeply.
 * @param {number[] | number} expectedOutput Expected output parameters after constant folding, or a
 *     scalar if the operation was calculated statically.
 * @param {string} message error message
 * @returns true if the explain output matches expectedOutput, and an assertion failure otherwise.
 */
function assertConstantFoldingResult(input, expectedOutput, message) {
    const buildExpressionFromArguments = (arr, op) => {
        if (Array.isArray(arr)) {
            return {
                [op]: arr.map(elt => buildExpressionFromArguments(elt, op))
            }
        } else if (typeof arr === 'string' || arr instanceof String) {
            return arr;
        } else {
            return {
                $const: arr
            }
        }
    };
    let pipeline = [
        {$group: {_id: {$add: buildExpressionFromArguments(input, "$add")}, sum: {$sum: 1}}},
    ];

    let result = db.runCommand(
        {explain: {aggregate: "coll", pipeline: pipeline, cursor: {}}, verbosity: 'queryPlanner'});

    assert(result.stages && result.stages[1] && result.stages[1].$group, result);
    const expected = buildExpressionFromArguments(expectedOutput, "$add");
    assert.eq(result.stages[1].$group._id, expected, message);
    return true;
}

assertConstantFoldingResult([1, 2, 3], 6, "All constants can fold.");
assertConstantFoldingResult([[1, 2], 3, 4, 5], 15, "Nested $adds can be folded away.");
assertConstantFoldingResult([1, 2, x], [3, x], "Constants can fold left-to-right.");
assertConstantFoldingResult([1, 2, x, 3], [3, x, 3], "Constants can fold up until a variable.");
assertConstantFoldingResult(
    [x, 1, 2, 3], [x, 1, 2, 3], "Variable on the left blocks folding constants.");

// Non-optimized comparisons -- make sure that non-optimized pipelines will give the same result as
// optimized ones.
})();