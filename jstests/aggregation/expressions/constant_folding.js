(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode() and assertArrayEq().

const collName = "jstests_aggregation_add";
const coll = db["collName"];
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

    assert(processedPipeline[0] && processedPipeline[0].$group)
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
}());

// Randomized property testing.
(function() {
load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode() and assertArrayEq().

const collName = "jstests_aggregation_add";
const coll = db["collName"];
coll.drop();

// TODO: SERVER-67282 Randomized property testing should work after SBE is updated to match classic
// engine.
db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true})

function assertPipelineCorrect(pipeline, v) {
    let optimizedResults = coll.aggregate(pipeline).toArray();
    db.adminCommand({
        configureFailPoint: 'disablePipelineOptimization',
        mode: 'alwaysOn',
    })
    let unoptimizedResults = coll.aggregate(pipeline).toArray();
    db.adminCommand({
        configureFailPoint: 'disablePipelineOptimization',
        mode: 'off',
    })
    assertArrayEq({
        actual: unoptimizedResults,
        expected: optimizedResults,
        extraErrorMsg: tojson({pipeline, v})
    });
}

/**
 * Randomized, property-based test of the left-to-right constant folding optimization. The purpose
 * of folding left-to-right is to preserve the same order-of-operations during ahead-of-time
 * constant folding that occurs during runtime execution.
 *
 * Given:
 *  - A random list of numbers of any type
 *  - A fieldpath reference placed at a random location in the list of numbers
 *  - A pipeline that performs an arithmetic operation over the list of arguments (fieldpath +
 *      numbers)
 * Prove:
 *  - The arithmetic operation produces the exact same result with and without optimizations.
 * @param {options} options
 */
function runRandomizedPropertyTest({op, min, max}) {
    // Function to generate random numbers of float, long, double, and NumberDecimal (with different
    // probabilities).
    const generateNumber = () => {
        const r = Math.random() * (max - min) + min;
        const t = Math.random();
        if (t < 0.7) {
            return r;
        }
        if (t < 0.85) {
            return NumberInt(Math.round(r));
        }
        if (t < 0.99) {
            return NumberLong(Math.round(r));
        }
        return NumberDecimal(String(r));
    };

    const generateNumberList = (length) => Array.from({length}, () => generateNumber(min, max));

    const numbers = generateNumberList(10)
    // Place a fieldpath reference randomly within the list of numbers to produce an argument list.
    const pos = Math.floor(numbers.length * Math.random());
    const args = [].concat(numbers.slice(0, pos), ["$v"], numbers.slice(pos));

    const pipeline = [{
        $group: {
            _id: {[op]: args},
            sum: {$sum: 1},
        },
    }];
    coll.drop();
    const v = generateNumber();
    coll.insert({v});
    assertPipelineCorrect(pipeline, v);
}

for (let i = 0; i < 100; i++) {
    runRandomizedPropertyTest({op: "$add", min: -314159255, max: 314159255});
    runRandomizedPropertyTest({op: "$multiply", min: -31415, max: 31415});
}
})();
