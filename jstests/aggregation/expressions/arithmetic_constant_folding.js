/**
 * Confirm the correctness of left-to-right associativity for arithmetic operations that take
 * multiple arguments.
 * @tags: [
 *  do_not_wrap_aggregations_in_facets,
 *  requires_pipeline_optimization,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode() and assertArrayEq().

const collName = jsTest.name();
const coll = db[collName];
coll.drop();

const $x = "$x";  // fieldpath to "block" constant folding

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

    let processedPipeline = getExplainedPipelineFromAggregation(db, db[collName], [
        {$group: {_id: buildExpressionFromArguments(input, op), sum: {$sum: 1}}},
    ]);

    assert(processedPipeline[0] && processedPipeline[0].$group);
    assert.eq(processedPipeline[0].$group._id, expected, message);

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
assertConstantFoldingResults([1, 2, $x],
                             [3, $x],
                             [2, $x],
                             "Constants should fold left-to-right before the first non-constant.");
assertConstantFoldingResults(
    [$x, 1, 2],
    [$x, 1, 2],
    [$x, 1, 2],
    "Constants should not fold left-to-right after the first non-constant.");
assertConstantFoldingResults(
    [1, $x, 2], [1, $x, 2], [1, $x, 2], "Constants should not fold across non-constants.");

assertConstantFoldingResults([5, 2, $x, 3, 4],
                             [7, $x, 3, 4],
                             [10, $x, 3, 4],
                             "Constants should fold up until a non-constant.");

assertConstantFoldingResults([$x, 1, 2, 3],
                             [$x, 1, 2, 3],
                             [$x, 1, 2, 3],
                             "Non-constant at start of operand list blocks folding constants.");

assertConstantFoldingResults([[1, 2, $x], 3, 4, $x, 5],
                             [[3, $x], 3, 4, $x, 5],
                             [[2, $x], 3, 4, $x, 5],
                             "Nested operation folds as expected.");

assertConstantFoldingResults(
    [1, 2, [1, 2, $x], 3, 4, $x, 5],
    [3, [3, $x], 3, 4, $x, 5],
    [2, [2, $x], 3, 4, $x, 5],
    "Nested operation folds along with outer operation following left-associative rules.");

assertConstantFoldingResults(
    [1, 2, [1, 2, $x, 5, 6], 3, 4, 5],
    [3, [3, $x, 5, 6], 3, 4, 5],
    [2, [2, $x, 5, 6], 3, 4, 5],
    "Nested operation folds along and outer operation does not fold past inner expression even without toplevel fieldpaths.");

assertConstantFoldingResults(
    [1, 2, $x, 4, [1, 2, $x, 5, 6], 3, 4, 5],
    [3, $x, 4, [3, $x, 5, 6], 3, 4, 5],
    [2, $x, 4, [2, $x, 5, 6], 3, 4, 5],
    "Nested operation folds along and even when fieldpath exists before it.");
}());

// Mixing $add and $multiply
(function() {
"use strict";
load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode() and assertArrayEq().

const collName = jsTest.name();
const coll = db[collName];
coll.drop();

const assertFoldedResult = (expr, expected, message) => {
    let processedPipeline = getExplainedPipelineFromAggregation(db, db[collName], [
        {$group: {_id: expr, sum: {$sum: 1}}},
    ]);
    const wrapLits = (arr) => {
        if (Array.isArray(arr)) {
            return arr.map(wrapLits);
        } else if (typeof arr === 'object') {
            let out = {};
            Object.keys(arr).forEach(k => {
                out[k] = wrapLits(arr[k]);
            });
            return out;
        } else if (typeof arr === 'string' || arr instanceof String) {
            return arr;
        } else {
            return {$const: arr};
        }
    };

    assert(processedPipeline[0] && processedPipeline[0].$group);
    assert.eq(processedPipeline[0].$group._id, wrapLits(expected), message);
};

assertFoldedResult({$add: [1, 2, {$multiply: [3, 4, "$x", 5, 6]}, 6, 7]},
                   {$add: [3, {$multiply: [12, "$x", 5, 6]}, 6, 7]},
                   "Multiply inside add will fold as much as it can.");

assertFoldedResult({$multiply: [1, 2, {$add: [3, 4, "$x", 5, 6]}, 6, 7]},
                   {$multiply: [2, {$add: [7, "$x", 5, 6]}, 6, 7]},
                   "Add inside multiply will fold as much as it can.");

assertFoldedResult({$add: [1, 2, {$multiply: [3, 4, 5, 6]}, 6, "$x", 7, 8]},
                   {$add: [369, "$x", 7, 8]},
                   "Multiply without fieldpath will fold away and add will continue folding.");

assertFoldedResult({$multiply: [1, 2, {$add: [3, 4, 5, 6]}, 6, "$x", 7, 8]},
                   {$multiply: [216, "$x", 7, 8]},
                   "Add without fieldpath will fold away and multiply will continue folding.");

assertFoldedResult(
    {$add: [1, 2, "$x", {$multiply: [3, 4, "$x", 5, 6]}, 6, 7, 8]},
    {$add: [3, "$x", {$multiply: [12, "$x", 5, 6]}, 6, 7, 8]},
    "Constant folding nested $multiply proceeds even after outer $add stops folding.");

assertFoldedResult(
    {$multiply: [1, 2, "$x", {$add: [3, 4, "$x", 5, 6]}, 6, 7, 8]},
    {$multiply: [2, "$x", {$add: [7, "$x", 5, 6]}, 6, 7, 8]},
    "Constant folding nested $add proceeds even after outer multiply stops folding.");
}());

// Regression tests for BFs related to SERVER-63099.
(function() {
"use strict";
// TODO: comparing NumberDecimals as equal doesn't work in the shell.
load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode() and assertArrayEq().

const coll = db[jsTest.name()];
coll.drop();

const makePipeline = (id) => [{$group: {_id: id, sum: {$sum: 1}}}];

// Non-optimized comparisons -- make sure that non-optimized pipelines will give the same result as
// optimized ones.
// This is a regression test for BF-24149.
coll.insert({_id: 0, v: NumberDecimal("917.6875119062092")});
coll.insert({_id: 1, v: NumberDecimal("927.3345924210555")});

const idToString = d => d._id.toJSON().$numberDecimal;

assertArrayEq({
    actual: coll.aggregate(makePipeline({$multiply: [-3.14159265859, "$v", -314159255]}))
                .toArray()
                .map(idToString),
    expected: [
        "915242528741.9469524422272990976000",
        "905721242210.0453137831269007622941",
    ]
});

// BF-24945
coll.drop();
coll.insert({x: 0, y: 4.1});
assert(numberDecimalsEqual(
    coll
        .aggregate(makePipeline(
            {$multiply: [NumberDecimal("-9.999999999999999999999999999999999E+6144"), "$x", "$y"]}))
        .toArray()[0]
        ._id,
    NumberDecimal(0)));
assertArrayEq({
    actual: coll.aggregate(makePipeline({
                    $multiply:
                        [NumberDecimal("-9.999999999999999999999999999999999E+6144"), "$y", "$x"]
                }))
                .toArray()
                .map(idToString),
    expected: ["NaN"]
});
}());
