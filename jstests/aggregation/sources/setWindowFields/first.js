/**
 * Test the behavior of $first.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const getParam = db.adminCommand({getParameter: 1, featureFlagWindowFunctions: 1});
jsTestLog(getParam);
const featureEnabled = assert.commandWorked(getParam).featureFlagWindowFunctions.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the window function feature flag is disabled");
    return;
}

const coll = db[jsTestName()];
coll.drop();

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

// Run the suite of partition and bounds tests against the $first function.
testAccumAgainstGroup(coll, "$first");

// Like most other window functions, the default window for $first is [unbounded, unbounded].
coll.drop();
assert.commandWorked(coll.insert([
    {x: 0, y: 0},
    {x: 1, y: 42},
    {x: 2, y: 67},
    {x: 3, y: 99},
    {x: 4, y: 20},
]));
let result = coll.aggregate([
                     {
                         $setWindowFields: {
                             sortBy: {x: 1},
                             output: {
                                 first: {$first: "$y"},
                             }
                         }
                     },
                     {$unset: "_id"},
                 ])
                 .toArray();
assert.sameMembers(result, [
    {x: 0, y: 0, first: 0},
    {x: 1, y: 42, first: 0},
    {x: 2, y: 67, first: 0},
    {x: 3, y: 99, first: 0},
    {x: 4, y: 20, first: 0},
]);

// A default value of NULL is returned if there is no first document.
coll.drop();
assert.commandWorked(coll.insert([
    {x: 1, y: 5},
    {x: 2, y: 4},
    {x: 3, y: 6},
    {x: 4, y: 5},
]));
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         sortBy: {x: 1},
                         partitionBy: "$x",
                         output: {
                             first: {$first: "$y", window: {documents: [-1, -1]}},
                         }
                     }
                 },
                 {$unset: "_id"},
             ])
             .toArray();
assert.sameMembers(result, [
    {x: 1, y: 5, first: null},
    {x: 2, y: 4, first: null},
    {x: 3, y: 6, first: null},
    {x: 4, y: 5, first: null},
]);

// Nonobject window fields cause parse errors
result = coll.runCommand({
    explain: {
        aggregate: coll.getName(),
        cursor: {},
        pipeline: [
            {
                $setWindowFields: {
                    sortBy: {x: 1},
                    output: {
                        first: {$first: "$y", window: [0, 1]},
                    }
                }
            },
        ]
    }
});
assert.commandFailedWithCode(result, ErrorCodes.FailedToParse, "'window' field must be an object");
})();
