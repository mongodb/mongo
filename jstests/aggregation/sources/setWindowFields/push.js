/**
 * Test that $push works as a window function.
 */
import {
    seedWithTickerData,
    testAccumAgainstGroup
} from "jstests/aggregation/extras/window_function_helpers.js";

const coll = db[jsTestName()];
coll.drop();

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

// Run the suite of partition and bounds tests against the $push function.
testAccumAgainstGroup(coll, "$push", []);

// Test $push with empty fields.
coll.drop();
assert.commandWorked(coll.insert([
    {_id: 0},
    {_id: 1, a: 'a'},
    {_id: 2},
]));
const result =
    coll.aggregate([{
            $setWindowFields:
                {sortBy: {_id: 1}, output: {b: {$push: '$a', window: {documents: [-1, 0]}}}}
        }])
        .toArray();
assert.eq(3, result.length, result);
assert.docEq({_id: 0, b: []}, result[0], result);
assert.docEq({_id: 1, a: 'a', b: ['a']}, result[1], result);
assert.docEq({_id: 2, b: ['a']}, result[2], result);
