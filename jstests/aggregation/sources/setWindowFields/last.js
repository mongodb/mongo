/**
 * Test the behavior of $last.
 */
import {seedWithTickerData, testAccumAgainstGroup} from "jstests/aggregation/extras/window_function_helpers.js";

const coll = db[jsTestName()];
coll.drop();

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

// Run the suite of partition and bounds tests against the $last function.
testAccumAgainstGroup(coll, "$last");

// Like most other window functions, the default window for $last is [unbounded, unbounded].
coll.drop();
assert.commandWorked(
    coll.insert([
        {x: 0, y: 0},
        {x: 1, y: 42},
        {x: 2, y: 67},
        {x: 3, y: 99},
        {x: 4, y: 20},
    ]),
);
let result = coll
    .aggregate([
        {
            $setWindowFields: {
                sortBy: {x: 1},
                output: {
                    last: {$last: "$y"},
                },
            },
        },
        {$unset: "_id"},
    ])
    .toArray();
assert.sameMembers(result, [
    {x: 0, y: 0, last: 20},
    {x: 1, y: 42, last: 20},
    {x: 2, y: 67, last: 20},
    {x: 3, y: 99, last: 20},
    {x: 4, y: 20, last: 20},
]);

// A default value of NULL is returned if there is no last document.
coll.drop();
assert.commandWorked(
    coll.insert([
        {x: 1, y: 5},
        {x: 2, y: 4},
        {x: 3, y: 6},
        {x: 4, y: 5},
    ]),
);
result = coll
    .aggregate([
        {
            $setWindowFields: {
                sortBy: {x: 1},
                partitionBy: "$x",
                output: {
                    last: {$last: "$y", window: {documents: [-1, -1]}},
                },
            },
        },
        {$unset: "_id"},
    ])
    .toArray();
assert.sameMembers(result, [
    {x: 1, y: 5, last: null},
    {x: 2, y: 4, last: null},
    {x: 3, y: 6, last: null},
    {x: 4, y: 5, last: null},
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
                        last: {$last: "$y", window: [0, 1]},
                    },
                },
            },
        ],
    },
});
assert.commandFailedWithCode(result, ErrorCodes.FailedToParse, "'window' field must be an object");

// A default value of NULL is returned if the expression is constant and the window is empty.
result = coll
    .aggregate([
        {
            $setWindowFields: {
                sortBy: {x: 1},
                output: {
                    last: {$last: "VAL", window: {documents: [2, 3]}},
                },
            },
        },
        {$unset: "_id"},
    ])
    .toArray();
assert.sameMembers(result, [
    {x: 1, y: 5, last: "VAL"},
    {x: 2, y: 4, last: "VAL"},
    {x: 3, y: 6, last: null},
    {x: 4, y: 5, last: null},
]);

result = coll
    .aggregate([
        {
            $setWindowFields: {
                sortBy: {x: 1},
                output: {
                    last: {$last: "VAL", window: {documents: [-3, -2]}},
                },
            },
        },
        {$unset: "_id"},
    ])
    .toArray();
assert.sameMembers(result, [
    {x: 1, y: 5, last: null},
    {x: 2, y: 4, last: null},
    {x: 3, y: 6, last: "VAL"},
    {x: 4, y: 5, last: "VAL"},
]);

// Assert NULL is returned if field is missing
coll.drop();
assert.commandWorked(
    coll.insert([
        {_id: 1, x: 1, y: 3},
        {_id: 2, x: 1},
        {_id: 3, x: 2, y: 6},
        {_id: 4, x: 2, y: 5},
    ]),
);
result = coll
    .aggregate([
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                partitionBy: "$x",
                output: {
                    last: {$last: "$y", window: {documents: [-1, 1]}},
                },
            },
        },
    ])
    .toArray();
assert.sameMembers(result, [
    {_id: 1, x: 1, y: 3, last: null},
    {_id: 2, x: 1, last: null},
    {_id: 3, x: 2, y: 6, last: 5},
    {_id: 4, x: 2, y: 5, last: 5},
]);

// Re-run query with unbounded window
result = coll
    .aggregate([
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                partitionBy: "$x",
                output: {
                    last: {$last: "$y", window: {}},
                },
            },
        },
    ])
    .toArray();
assert.sameMembers(result, [
    {_id: 1, x: 1, y: 3, last: null},
    {_id: 2, x: 1, last: null},
    {_id: 3, x: 2, y: 6, last: 5},
    {_id: 4, x: 2, y: 5, last: 5},
]);
