/**
 * Test that $count works as a window function.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // documentEq

const coll = db[jsTestName()];
coll.drop();

const nDocs = 10;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({
        one: i,
        partition: i % 2,
        partitionSeq: Math.trunc(i / 2),
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

// Run an unpartitioned count query using the window bounds specified as [lower, upper].
function runCountQuery(lower, upper) {
    return coll
        .aggregate([
            {
                $setWindowFields: {
                    sortBy: {one: 1},
                    output: {a: {$count: {}, window: {documents: [lower, upper]}}}
                }
            },
            {$sort: {_id: 1}}
        ])
        .toArray();
}

// Test left unbounded count.
let result = runCountQuery("unbounded", "unbounded");
verifyResults(result, function(num, baseObj) {
    baseObj.a = nDocs;
    return baseObj;
});

// Test left unbounded count.
result = runCountQuery("unbounded", "current");
verifyResults(result, function(num, baseObj) {
    baseObj.a = baseObj.one + 1;
    return baseObj;
});

// test sliding window using only previous vals.
result = runCountQuery(-1, "current");
verifyResults(result, function(num, baseObj) {
    baseObj.a = (baseObj.one == 0) ? 1 : 2;
    return baseObj;
});

// test sliding window using future vals.
result = runCountQuery(-1, 1);
verifyResults(result, function(num, baseObj) {
    if (baseObj.one == 0 || baseObj.one == nDocs - 1) {
        baseObj.a = 2;
    } else {
        baseObj.a = 3;
    }
    return baseObj;
});

// Run an count query partitioned over "$partition" using the window bounds specified as
// [lower, upper].
function runPartitionedCountQuery(lower, upper) {
    return coll
        .aggregate([
            {
                $setWindowFields: {
                    partitionBy: "$partition",
                    sortBy: {one: 1},
                    output: {a: {$count: {}, window: {documents: [lower, upper]}}}
                }
            },
            {$sort: {_id: 1}}
        ])
        .toArray();
}

// Test partitioned unbounded count.
result = runPartitionedCountQuery("unbounded", "unbounded");
verifyResults(result, function(num, baseObj) {
    baseObj.a = nDocs / 2;
    return baseObj;
});

// Test partitioned left unbounded count.
result = runPartitionedCountQuery("unbounded", "current");
verifyResults(result, function(num, baseObj) {
    baseObj.a = baseObj.partitionSeq + 1;
    return baseObj;
});

// test partitioned sliding window using only previous vals.
result = runPartitionedCountQuery(-1, "current");
verifyResults(result, function(num, baseObj) {
    baseObj.a = (baseObj.partitionSeq == 0) ? 1 : 2;
    return baseObj;
});

// test partitioned sliding window using future vals.
result = runPartitionedCountQuery(-1, 1);
verifyResults(result, function(num, baseObj) {
    if (baseObj.partitionSeq == 0 || baseObj.partitionSeq == Math.trunc(nDocs / 2) - 1) {
        baseObj.a = 2;
    } else {
        baseObj.a = 3;
    }
    return baseObj;
});
})();
