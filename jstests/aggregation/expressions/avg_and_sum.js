/*
 * Additional tests for $avg and $sum when used as expressions.
 */

var coll = db.collection;
assert(coll.drop());

/*
 * Compares the expected result with the result of $avg in the agg pipeline.
 */
function assertResultAvg(expected, field) {
    assert.eq(expected, coll.aggregate({$project: {a: {$avg: field}}}).toArray()[0].a);
}

/*
 * Compares the expected result with the result of $sum in the agg pipeline.
 */
function assertResultSum(expected, field) {
    assert.eq(expected, coll.aggregate({$project: {a: {$sum: field}}}).toArray()[0].a);
}

coll.insertOne({
    "null": null,
    "undefined": undefined,
    "string": "hello world",
    "num": -1,
    "num2": 13.4,
    "arrayEmpty": [],
    "array1": [1, 2, 3],
    "array2": [1, 2, 3, "string"],
    "array3": [12.4, 5.6, 9.805],
    "arrayNested1": [[1, 2, 3], new Map()],
    "arrayNested2": [[1, 2, 3], 4, null]
});

// Single non-array input.
assertResultAvg(null, "$null");
assertResultAvg(null, "$undefined");
assertResultAvg(null, "$doesNotExist");
assertResultAvg(null, "$string");
assertResultAvg(-1, "$num");
assertResultAvg(13.4, "$num2");

assertResultSum(0, "$null");
assertResultSum(0, "$undefined");
assertResultSum(0, "$doesNotExist");
assertResultSum(0, "$string");
assertResultSum(-1, "$num");
assertResultSum(13.4, "$num2");

// Single array or multiple inputs.
assertResultAvg(null, "$arrEmpty");
assertResultAvg(2, "$array1");
assertResultAvg(2, "$array2");
assertResultAvg(9.268333333333333, "$array3");
assertResultAvg(null, "$arrayNested1");
assertResultAvg(4, "$arrayNested2");

assertResultSum(0, "$arrEmpty");
assertResultSum(6, "$array1");
assertResultSum(6, "$array2");
assertResultSum(27.805, "$array3");
assertResultAvg(null, "$arrayNested1");
assertResultSum(4, "$arrayNested2");

assert(coll.drop());
