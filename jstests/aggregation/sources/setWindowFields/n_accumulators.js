/**
 * Test that the 'n' family of accumulators work as window functions.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const coll = db[jsTestName()];
coll.drop();

// TODO SERVER-57886: Add test cases for $top/$bottom/$topN/$bottomN window functions.
const nAccumulators = ["$minN", "$maxN", "$firstN", "$lastN"];
const isExactTopNEnabled = db.adminCommand({getParameter: 1, featureFlagExactTopNAccumulator: 1})
                               .featureFlagExactTopNAccumulator.value;

if (!isExactTopNEnabled) {
    // Verify that $minN/$maxN/$firstN/$lastN cannot be used if the feature flag is set to false and
    // ignore the rest of the test.
    for (const acc of nAccumulators) {
        assert.commandFailedWithCode(coll.runCommand("aggregate", {
            pipeline: [{
                $setWindowFields: {
                    sortBy: {ts: 1},
                    output: {outputField: {[acc]: {n: 3, input: "$foo"}}},
                }
            }],
            cursor: {}
        }),
                                     ErrorCodes.FailedToParse);
    }
    return;
}

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

for (const acc of nAccumulators) {
    for (const nValue of [4, 7, 12]) {
        jsTestLog("Testing accumulator " + tojson(acc) + " with 'n' set to " + tojson(nValue));
        testAccumAgainstGroup(coll, acc, [], {input: "$price", n: nValue});
    }

    // Verify that the accumulator will not throw if the 'n' expression evaluates to a constant.
    const pipeline = [
        {
            $setWindowFields: {
                partitionBy: "$ticker",
                sortBy: {_id: 1},
                output: {res: {[acc]: {n: {$add: [1, 2]}, input: "$price"}}}
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

    // The parsers for $minN/$maxN and $firstN/$lastN will use different codes to indicate a
    // parsing error due to a non-object specification.
    const expectedCode = (acc === "$minN" || acc === "$maxN") ? 5787900 : 5787801;

    // Invalid/missing accumulator specification.
    testError({[acc]: "non object"}, expectedCode);
    testError({window: {documents: [-1, 1]}}, ErrorCodes.FailedToParse);
    testError({[acc]: {n: 2}, window: {documents: [-1, 1]}}, 5787907);
    testError({[acc]: {input: "$foo"}, window: {documents: [-1, 1]}}, 5787906);
    testError({[acc]: {input: "$foo", n: 2.1}, window: {documents: [-1, 1]}}, 5787903);

    // Invalid window specification.
    testError({[acc]: {input: "$foo", n: 2.0}, window: [-1, 1]}, ErrorCodes.FailedToParse);

    // Non constant argument for 'n'.
    testError({[acc]: {input: "$foo", n: "$a"}, window: {documents: [-1, 1]}}, 5787902);

    // n = 0
    testError({[acc]: {input: "$foo", n: 0}, window: {documents: [-1, 1]}}, 5787908);

    // n < 0
    testError({[acc]: {input: "$foo", n: -100}, window: {documents: [-1, 1]}}, 5787908);
}
})();