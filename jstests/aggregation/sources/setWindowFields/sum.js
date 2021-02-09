/*
 * Test that $sum works as a window function.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // documentEq
const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagWindowFunctions: 1}))
        .featureFlagWindowFunctions.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the window function feature flag is disabled");
    return;
}

const coll = db[jsTestName()];
coll.drop();

for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({one: i, two: i * 2, arr: [{first: i}, {second: 0}]}));
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
let result = coll.aggregate([
                     sortStage,
                     {
                         $setWindowFields: {
                             sortBy: {one: 1},
                             output: {a: {$sum: {input: 1, documents: ["unbounded", "current"]}}}
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
                    output: {a: {$sum: {input: "$one", documents: ["unbounded", "current"]}}}
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
                             a: {$sum: {input: "$one", documents: ["unbounded", "current"]}},
                             b: {$sum: {input: "$two", documents: ["unbounded", "current"]}}
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
                    output: {one: {$sum: {input: "$one", documents: ["unbounded", "current"]}}}
                }
            }
        ])
        .toArray();
verifyResults(result, function(num, baseObj) {
    baseObj.one = firstSum(num);
    return baseObj;
});
// TODO: SERVER-54340 Enable these tests.
// Test that we can set a sub-field in each document in an array.
// result =
// coll.aggregate([
// sortStage,
// {
// $setWindowFields:
// {sortBy: {one: 1}, output: {"arr.a": {$sum: {input: "$one", documents: ["unbounded",
// "current"]}}}}
// }
// ])
// .toArray();
// verifyResults(result, function(num, baseObj) {
// baseObj.arr[0] = {first: baseObj.arr[0].first, a: firstSum(num)}
// baseObj.arr[1] = {second: 0, a: firstSum(num)};
// return baseObj;
// });

// // Test that we can set a nested field.
// result =
// coll.aggregate([
// sortStage,
// {
// $setWindowFields:
// {sortBy: {one: 1}, output:
// {"a.b": {$sum: {input: "$one", documents: ["unbounded", "current"]}}}}
// }
// ])
// .toArray();
// verifyResults(result, function(num, baseObj) {
// baseObj.a = {b: firstSum(num)};
// return baseObj;
// });
})();
