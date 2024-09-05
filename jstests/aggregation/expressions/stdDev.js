/*
 * Additional tests for $stdDevPop and $stdDevSamp when used as aggregation expressions.
 */

var coll = db.collection;
assert(coll.drop());

coll.insertOne({
    "num": 1,
    "decimal": 1.23,
    "null": null,
    "undefined": undefined,
    "arrSimple": [1, 2, 3, 4],
    "arrEmpty": [],
    "arrMixed": [1, 2, 3, 4, "hello", null],
    "arrMixed2": [[], 7, 19, 21, new Map()],
    "arrDecimal": [5.6, 3.4, 5.6, 8.23],
    "arrNested": [[1]],
    "arrNested2": [[[], 7, 19, 21, new Map()]],
});

/*
 * Compares the expected result with the result of $stdDevPop in the aggregation pipeline.
 */
function assertResultStdDevPop(expected, field) {
    assert.eq(expected, coll.aggregate({$project: {a: {$stdDevPop: field}}}).toArray()[0].a);
}

/*
 * Compares the expected result with the result of $stdDevSamp in the aggregation pipeline.
 */
function assertResultStdDevSamp(expected, field) {
    assert.eq(expected, coll.aggregate({$project: {a: {$stdDevSamp: field}}}).toArray()[0].a);
}

// Single non-array input.
assertResultStdDevPop(0, "$num");
assertResultStdDevPop(0, ["$num"]);
assertResultStdDevPop(0, "$decimal");
assertResultStdDevPop(null, "$null");
assertResultStdDevPop(null, "$undefined");
assertResultStdDevPop(null, "$doesNotExist");

assertResultStdDevSamp(null, "$num");
assertResultStdDevSamp(null, ["$num"]);
assertResultStdDevSamp(null, "$decimal");
assertResultStdDevSamp(null, "$null");
assertResultStdDevSamp(null, "$undefined");
assertResultStdDevSamp(null, "$doesNotExist");

// Single or nested array input.
assertResultStdDevPop(1.118033988749895, "$arrSimple");
assertResultStdDevPop(null, "$arrEmpty");
assertResultStdDevPop(1.118033988749895, "$arrMixed");
assertResultStdDevPop(6.182412330330469, "$arrMixed2");
assertResultStdDevPop(1.7110431759602098, "$arrDecimal");
assertResultStdDevPop(null, "$arrNested");
assertResultStdDevPop(null, "$arrNested2");

assertResultStdDevSamp(1.2909944487358056, "$arrSimple");
assertResultStdDevSamp(null, "$arrEmpty");
assertResultStdDevSamp(1.2909944487358056, "$arrMixed");
assertResultStdDevSamp(7.571877794400365, "$arrMixed2");
assertResultStdDevSamp(1.9757424764713987, "$arrDecimal");
assertResultStdDevSamp(null, "$arrNested2");
assertResultStdDevSamp(null, "$arrNested");

assert(coll.drop());
