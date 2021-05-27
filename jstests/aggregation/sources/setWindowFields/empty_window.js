/**
 * Test the default behavior of window functions when no documents fall in the window.
 */
(function() {
"use strict";

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({_id: 0, ticker: "MDB", price: 1000, ts: new Date()}));

// Over a non-existent field.
let results =
    coll.aggregate([{
            $setWindowFields: {
                sortBy: {ts: 1},
                output: {
                    defaultStdDev:
                        {$stdDevPop: "$noField", window: {documents: ["unbounded", "current"]}},
                    defaultSum: {$sum: "$noField", window: {documents: ["unbounded", "current"]}},
                    defaultAvg: {$avg: "$noField", window: {documents: ["unbounded", "current"]}},
                    defaultMin: {$min: "$noField", window: {documents: ["unbounded", "current"]}},
                    defaultMax: {$max: "$noField", window: {documents: ["unbounded", "current"]}},
                }
            }
        }])
        .toArray();
assert.eq(null, results[0].defaultStdDev);
assert.eq(null, results[0].defaultAvg);
assert.eq(null, results[0].defaultMin);
assert.eq(null, results[0].defaultMax);
// $sum is unique as its default value is 0.
assert.eq(0, results[0].defaultSum);

// Over a window with no documents.
results =
    coll.aggregate([{
            $setWindowFields: {
                sortBy: {ts: 1},
                output: {
                    defaultStdDev: {$stdDevPop: "$price", window: {documents: ["unbounded", -1]}},
                    defaultSum: {$sum: "$price", window: {documents: ["unbounded", -1]}},
                    defaultAvg: {$avg: "$price", window: {documents: ["unbounded", -1]}},
                    defaultMin: {$min: "$price", window: {documents: ["unbounded", -1]}},
                    defaultMax: {$max: "$price", window: {documents: ["unbounded", -1]}},
                }
            }
        }])
        .toArray();
assert.eq(null, results[0].defaultStdDev);
assert.eq(null, results[0].defaultAvg);
assert.eq(null, results[0].defaultMin);
assert.eq(null, results[0].defaultMax);
// $sum is unique as its default value is 0.
assert.eq(0, results[0].defaultSum);
})();
