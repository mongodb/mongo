/**
 * Test that the 'n' family of accumulators work as window functions.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const coll = db[jsTestName()];
coll.drop();

const needsSortBy = (op) => ({
    $minN: false,
    $maxN: false,
    $firstN: false,
    $lastN: false,
    $topN: true,
    $bottomN: true,
    $top: true,
    $bottom: true,
}[op]);

// A map from the accumulator name to a function that ignores the parameters it doesn't need and
// generates a valid accumulator spec.
const simple = (n, input) => ({n, input: "$" + input});
const topBottomN = (n, output) => ({n, output: "$" + output, sortBy: {[output]: 1}});
const topBottom = (n, output) => ({output: "$" + output, sortBy: {[output]: 1}});
const nAccumulators = {
    $minN: simple,
    $maxN: simple,
    $firstN: simple,
    $lastN: simple,
    $topN: topBottomN,
    $bottomN: topBottomN,
    $top: topBottom,
    $bottom: topBottom,
};

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

for (const acc of Object.keys(nAccumulators)) {
    for (const nValue of [4, 7, 12]) {
        jsTestLog("Testing accumulator " + tojson(acc) + " with 'n' set to " + tojson(nValue));
        const noValue = (acc === "$top" || acc === "$bottom") ? null : [];
        testAccumAgainstGroup(coll, acc, noValue, nAccumulators[acc](nValue, "price"));
    }

    // Verify that the accumulator will not throw if the 'n' expression evaluates to a constant.
    const pipeline = [
        {
            $setWindowFields: {
                partitionBy: "$ticker",
                sortBy: {_id: 1},
                output: {res: {[acc]: nAccumulators[acc]({$add: [1, 2]}, "price")}}
            },
        },
    ];

    assert.doesNotThrow(() => coll.aggregate(pipeline).toArray());

    // Error cases.
    function testError(accSpec, expectedCode) {
        assert.throwsWithCode(() => coll.aggregate([{
            $setWindowFields: {
                partitionBy: "$ticket",
                sortBy: {ts: 1},
                output: {outputField: accSpec},
            }
        }]),
                              expectedCode);
    }

    // The parsers for $minN/$maxN and $firstN/$lastN will use different codes to indicate a
    // parsing error due to a non-object specification.
    const expectedCode = (acc === "$minN" || acc === "$maxN") ? 5787900 : 5787801;

    if (!needsSortBy(acc)) {
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

        // Can't reference partition key.
        testError({[acc]: {input: "$foo", n: "$ticket"}, window: {documents: [-1, 1]}}, 5787902);

        // n = 0
        testError({[acc]: {input: "$foo", n: 0}, window: {documents: [-1, 1]}}, 5787908);

        // n < 0
        testError({[acc]: {input: "$foo", n: -100}, window: {documents: [-1, 1]}}, 5787908);
    } else if (acc == "$topN" || acc == "$bottomN") {
        // TODO SERVER-59327 combine error codes if we decide to use the same parser function.
        // Invalid/missing accumulator specification.
        const sortBy = {"foo": 1};
        const output = "$foo";
        testError({[acc]: "non object"}, 5788001);
        testError({window: {documents: [-1, 1]}}, ErrorCodes.FailedToParse);
        testError({[acc]: {n: 2, sortBy}, window: {documents: [-1, 1]}}, 5788004);
        testError({[acc]: {output, sortBy}, window: {documents: [-1, 1]}}, 5788003);
        testError({[acc]: {output, sortBy, n: 2.1}, window: {documents: [-1, 1]}}, 5787903);

        // Missing sortBy.
        testError({[acc]: {output, n: 2}, window: {documents: [-1, 1]}}, 5788005);

        // Invalid window specification.
        testError({[acc]: {output, sortBy, n: 2.0}, window: [-1, 1]}, ErrorCodes.FailedToParse);

        // Non constant argument for 'n'.
        testError({[acc]: {output, sortBy, n: "$a"}, window: {documents: [-1, 1]}}, 5787902);

        // Can't reference partition key.
        testError({[acc]: {output, sortBy, n: "$ticket"}, window: {documents: [-1, 1]}}, 5787902);

        // n = 0
        testError({[acc]: {output, sortBy, n: 0}, window: {documents: [-1, 1]}}, 5787908);

        // n < 0
        testError({[acc]: {output, sortBy, n: -100}, window: {documents: [-1, 1]}}, 5787908);
    } else {
        // $top/$bottom parsing tests.
        const sortBy = {"foo": 1};
        const output = "$foo";
        testError({[acc]: "non object"}, 5788001);
        testError({window: {documents: [-1, 1]}}, ErrorCodes.FailedToParse);
        testError({[acc]: {sortBy}, window: {documents: [-1, 1]}}, 5788004);
        testError({[acc]: {n: 2, output, sortBy}, window: {documents: [-1, 1]}}, 5788002);

        // Missing sortBy.
        testError({[acc]: {output}, window: {documents: [-1, 1]}}, 5788005);
    }
}
})();
