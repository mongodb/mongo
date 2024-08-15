/**
 * Test that standard deviation works as a window function.
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

// Run the suite of partition and bounds tests against the $stdDevPop function.
testAccumAgainstGroup(coll, "$stdDevPop");
// Run the suite of partition and bounds tests against the $stdDevSamp function.
testAccumAgainstGroup(coll, "$stdDevSamp");

// Test that $stdDevPop and $stdDevSamp return null for windows which do not contain numeric values.
let results =
    coll.aggregate([
            {$addFields: {str: "hiya"}},
            {
                $setWindowFields: {
                    sortBy: {ts: 1},
                    output: {
                        stdDevPop:
                            {$stdDevPop: "$str", window: {documents: ["unbounded", "current"]}},
                        stdDevSamp:
                            {$stdDevSamp: "$str", window: {documents: ["unbounded", "current"]}},
                    }
                }
            }
        ])
        .toArray();
for (let index = 0; index < results.length; index++) {
    assert.eq(null, results[index].stdDevPop);
    assert.eq(null, results[index].stdDevSamp);
}

// Test leading non-finite value generates correct result.
for (let nonFinite of [NaN, Infinity]) {
    assert.eq(true, coll.drop());
    assert.commandWorked(coll.insertMany([
        {_id: 0, a: nonFinite},
        {_id: 1, a: 1},
        {_id: 2, a: 2},
        {_id: 3, a: nonFinite},
    ]));
    const cursor = coll.aggregate([
        {
            $setWindowFields: {
                partitionBy: null,
                sortBy: {_id: 1},
                output: {
                    b: {$stdDevSamp: "$a", window: {documents: [1, 2]}},
                    c: {$stdDevPop: "$a", window: {documents: [1, 2]}},
                }
            }
        },
        {
            $project: {
                stdDevSamp: {$trunc: ["$b", 2]},
                stdDevPop: {$trunc: ["$c", 2]},
                _id: 0,
            }
        },
    ]);
    let result = cursor.next();
    assert.docEq({stdDevSamp: 0.7, stdDevPop: 0.5}, result, result);
    result = cursor.next();
    assert.docEq({stdDevSamp: null, stdDevPop: null}, result, result);
}
