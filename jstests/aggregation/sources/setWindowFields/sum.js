/**
 * Test that $sum works as a window function.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");
load("jstests/aggregation/extras/utils.js");  // documentEq

const coll = db[jsTestName()];
coll.drop();

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

// Run the suite of partition and bounds tests against the $sum function.
testAccumAgainstGroup(coll, "$sum", 0);

coll.drop();

const nDocs = 10;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({
        one: i,
        two: i * 2,
        simpleArr: [1, 2, 3],
        docArr: [{first: i}, {second: 0}],
        nestedDoc: {1: {2: {3: 1}}},
        mixed: (i % 2) ? null : i,
    }));
}

const origDocs = coll.find().sort({_id: 1});
function verifyResults(results, valueFunction) {
    for (let i = 0; i < results.length; i++) {
        // Use Object.assign to make a copy instead of pass a reference.
        const correctDoc = valueFunction(i, Object.assign({}, origDocs[i]));
        assert(documentEq(correctDoc, results[i]),
               "Got: " + tojson(results[i]) + "\nExpected: " + tojson(correctDoc) +
                   "\n at position " + i + "\n");
    }
}

function firstSum(topNum) {
    return (topNum * (topNum + 1)) / 2;
}

function secondSum(topNum) {
    return topNum + (topNum * topNum);
}

const sortStage = {
    $sort: {_id: 1}
};

// Test using $sum to count.
let result =
    coll.aggregate([
            sortStage,
            {
                $setWindowFields: {
                    sortBy: {one: 1},
                    output: {a: {$sum: 1, window: {documents: ["unbounded", "current"]}}}
                }
            }
        ])
        .toArray();
verifyResults(result, function(num, baseObj) {
    baseObj.a = num + 1;
    return baseObj;
});

// Test that we can sum over one field.
result =
    coll.aggregate([
            sortStage,
            {
                $setWindowFields: {
                    sortBy: {one: 1},
                    output: {a: {$sum: "$one", window: {documents: ["unbounded", "current"]}}}
                }
            }
        ])
        .toArray();
verifyResults(result, function(num, baseObj) {
    baseObj.a = firstSum(num);
    return baseObj;
});

// Test that we can sum over two fields.
result = coll.aggregate([
                 sortStage,
                 {
                     $setWindowFields: {
                         sortBy: {one: 1},
                         output: {
                             a: {$sum: "$one", window: {documents: ["unbounded", "current"]}},
                             b: {$sum: "$two", window: {documents: ["unbounded", "current"]}}
                         }
                     }
                 }
             ])
             .toArray();
verifyResults(result, function(num, baseObj) {
    baseObj.a = firstSum(num);
    baseObj.b = secondSum(num);
    return baseObj;
});

// Test that we can overwrite an existing field.
result =
    coll.aggregate([
            sortStage,
            {
                $setWindowFields: {
                    sortBy: {one: 1},
                    output: {one: {$sum: "$one", window: {documents: ["unbounded", "current"]}}}
                }
            }
        ])
        .toArray();
verifyResults(result, function(num, baseObj) {
    baseObj.one = firstSum(num);
    return baseObj;
});

// Test that we can set a sub-field in each document in an array.
result =
    coll.aggregate([
            sortStage,
            {
                $setWindowFields: {
                    sortBy: {one: 1},
                    output:
                        {"docArr.a":
                             {$sum: "$one", window: {documents: ["unbounded", "current"]}}}
                }
            }
        ])
        .toArray();
verifyResults(result, function(num, baseObj) {
    baseObj.docArr =
        [{first: baseObj.docArr[0].first, a: firstSum(num)}, {second: 0, a: firstSum(num)}];
    return baseObj;
});

// Test that we can set multiple numeric sub-fields in each element in an array.
result =
    coll.aggregate([
            sortStage,
            {
                $setWindowFields: {
                    sortBy: {one: 1},
                    output: {
                        "docArr.1": {$sum: "$one", window: {documents: ["unbounded", "current"]}},
                        "docArr.2": {$sum: "$one", window: {documents: ["unbounded", "current"]}},
                        "simpleArr.1":
                            {$sum: "$one", window: {documents: ["unbounded", "current"]}}
                    }
                }
            }
        ])
        .toArray();
verifyResults(result, function(num, baseObj) {
    const newObj = {1: firstSum(num)};
    baseObj.docArr = [
        {first: baseObj.docArr[0].first, 1: firstSum(num), 2: firstSum(num)},
        {second: baseObj.docArr[1].second, 1: firstSum(num), 2: firstSum(num)}
    ];
    baseObj.simpleArr = Array.apply(null, Array(baseObj.simpleArr.length)).map(_ => newObj);
    return baseObj;
});

// Test that we can set a nested field.
result =
    coll.aggregate([
            sortStage,
            {
                $setWindowFields: {
                    sortBy: {one: 1},
                    output:
                        {"a.b": {$sum: "$one", window: {documents: ["unbounded", "current"]}}}
                }
            }
        ])
        .toArray();
verifyResults(result, function(num, baseObj) {
    baseObj.a = {b: firstSum(num)};
    return baseObj;
});

// Test that we can set multiple fields/sub-fields of different types at once.
result =
    coll.aggregate([
            sortStage,
            {
                $setWindowFields: {
                    sortBy: {one: 1},
                    output: {
                        "a": {$sum: "$one", window: {documents: ["unbounded", "current"]}},
                        "newField.a": {$sum: "$two", window: {documents: ["unbounded", "current"]}},
                        "simpleArr.0.b":
                            {$sum: "$one", window: {documents: ["unbounded", "current"]}},
                        "nestedDoc.1.2.a":
                            {$sum: "$one", window: {documents: ["unbounded", "current"]}}
                    }
                }
            }
        ])
        .toArray();
verifyResults(result, function(num, baseObj) {
    const newObj = {0: {b: firstSum(num)}};
    baseObj.a = firstSum(num);
    baseObj.newField = {a: secondSum(num)};
    baseObj.simpleArr = Array.apply(null, Array(baseObj.simpleArr.length)).map(_ => newObj);
    baseObj.nestedDoc = {1: {2: {3: 1, a: firstSum(num)}}};
    return baseObj;
});

// Test $sum over a non-removable lookahead window.
result = coll.aggregate([
                 sortStage,
                 {
                     $setWindowFields: {
                         sortBy: {one: 1},
                         output: {one: {$sum: "$one", window: {documents: ["unbounded", 1]}}}
                     }
                 }
             ])
             .toArray();
verifyResults(result, function(num, baseObj) {
    baseObj.one = firstSum(num);
    if (num < (nDocs - 1))
        baseObj.one += (num + 1);
    return baseObj;
});

// Test $sum over a non-removable window whose upper bound is behind the current.
result = coll.aggregate([
                 sortStage,
                 {
                     $setWindowFields: {
                         sortBy: {one: 1},
                         output: {one: {$sum: "$one", window: {documents: ["unbounded", -1]}}}
                     }
                 }
             ])
             .toArray();
verifyResults(result, function(num, baseObj) {
    baseObj.one = firstSum(num);
    // Subtract the "current" value from the accumulation.
    baseObj.one -= num;
    return baseObj;
});

// Test that non-numeric types do not contribute to the sum.
result = coll.aggregate([
                 sortStage,
                 {
                     $setWindowFields: {
                         sortBy: {one: 1},
                         output: {
                             mixedTypeSum:
                                 {$sum: "$mixed", window: {documents: ["unbounded", "current"]}}
                         }
                     }
                 }
             ])
             .toArray();
verifyResults(result, function(num, baseObj) {
    // The 'mixed' field contains alternating null and integers, manually calculate the running sum
    // for each document.
    baseObj.mixedTypeSum = 0;
    for (let i = 0; i <= num; i++) {
        baseObj.mixedTypeSum += (i % 2) ? 0 : i;
    }
    return baseObj;
});
})();
