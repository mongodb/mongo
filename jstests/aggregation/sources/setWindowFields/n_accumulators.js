/**
 * Test that the 'n' family of accumulators work as window functions.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const coll = db[jsTestName()];
coll.drop();

const isExactTopNEnabled = db.adminCommand({getParameter: 1, featureFlagExactTopNAccumulator: 1})
                               .featureFlagExactTopNAccumulator.value;

if (!isExactTopNEnabled) {
    // Verify that $minN/$maxN cannot be used if the feature flag is set to false and ignore the
    // rest of the test.
    assert.commandFailedWithCode(coll.runCommand("aggregate", {
        pipeline: [{
            $setWindowFields: {
                sortBy: {ts: 1},
                output: {outputField: {$minN: {n: 3, output: "$foo"}}},
            }
        }],
        cursor: {}
    }),
                                 5788502);
    return;
}

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

// TODO SERVER-57884: Add test cases for $firstN/$lastN window functions.
// TODO SERVER-57886: Add test cases for $top/$bottom/$topN/$bottomN window functions.
for (const acc of ["$minN", "$maxN"]) {
    for (const nValue of [4, 7, 12]) {
        jsTestLog("Testing accumulator " + tojson(acc) + " with 'n' set to " + tojson(nValue));
        testAccumAgainstGroup(coll, acc, [], {output: "$price", n: nValue});
    }

    // Verify that the accumulator will not throw if the 'n' expression evaluates to a constant.
    const pipeline = [
        {
            $setWindowFields: {
                partitionBy: "$ticker",
                sortBy: {_id: 1},
                output: {res: {[acc]: {n: {$add: [1, 2]}, output: "$price"}}}
            },
        },
    ];

    assert.doesNotThrow(() => coll.aggregate(pipeline).toArray());

    // Error cases.
    function testError(accSpec, expectedCode) {
        assert.throwsWithCode(() => coll.aggregate([{
            $setWindowFields: {
                sortBy: {ts: 1},
                output: {outputField: accSpec},
            }
        }]),
                              expectedCode);
    }
    // Invalid/missing accumulator specification.
    testError({[acc]: "non object"}, 5787900);
    testError({window: {documents: [-1, 1]}}, ErrorCodes.FailedToParse);
    testError({[acc]: {n: 2}, window: {documents: [-1, 1]}}, 5787907);
    testError({[acc]: {output: "$foo"}, window: {documents: [-1, 1]}}, 5787906);
    testError({[acc]: {output: "$foo", n: 2.1}, window: {documents: [-1, 1]}}, 5787903);

    // Invalid window specification.
    testError({[acc]: {output: "$foo", n: 2.0}, window: [-1, 1]}, ErrorCodes.FailedToParse);

    // Non constant argument for 'n'.
    testError({[acc]: {output: "$foo", n: "$a"}, window: {documents: [-1, 1]}}, 5787902);
}
})();