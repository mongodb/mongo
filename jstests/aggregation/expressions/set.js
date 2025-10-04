/**
 * Test the set expressions.
 */
import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {checkSbeCompletelyDisabled} from "jstests/libs/query/sbe_util.js";

const isMultiversion =
    Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || Boolean(TestData.multiversionBinVersion);

const coll = db.expression_set;
coll.drop();

assert.commandWorked(
    coll.insert([
        {_id: 0, arr1: [1, 2, 3], arr2: [2, 3, 4]},
        {_id: 1, arr1: [1, 2, 3], arr2: [4, 5, 6]},
        {_id: 2, arr1: [1, 2, 3], arr2: []},
        {_id: 3, arr1: [], arr2: [4, 5, 6]},
        {_id: 4, arr1: [1, 2, 3], arr2: [2, 3]},
        {_id: 5, arr1: [2, 3], arr2: [2, 3, 4]},
        {_id: 6, arr1: [1, 2, 3], arr2: [1, 2, 3]},
        {_id: 7, arr1: [1, 2, 3], arr2: [1, 1, 2, 2, 3, 3]},
    ]),
);

// The order of the output array elements is undefined for $setUnion, $setDifference and
// $setIntersection expressions. Hence we do a sort operation to get a consistent order.
const sortSetFields = (doc) => {
    let result = {};
    for (const key in doc) {
        if (doc.hasOwnProperty(key)) {
            const value = doc[key];
            result[key] = Array.isArray(value) ? value.sort() : value;
        }
    }
    return result;
};

const runTest = function (pipeline, expectedResult) {
    pipeline.push({$sort: {_id: 1}});
    const result = coll.aggregate(pipeline).toArray();
    assert.eq(expectedResult, result.map(sortSetFields));
};

runTest(
    [{$project: {union: {$setUnion: ["$arr1", "$arr2"]}}}],
    [
        {_id: 0, union: [1, 2, 3, 4]},
        {_id: 1, union: [1, 2, 3, 4, 5, 6]},
        {_id: 2, union: [1, 2, 3]},
        {_id: 3, union: [4, 5, 6]},
        {_id: 4, union: [1, 2, 3]},
        {_id: 5, union: [2, 3, 4]},
        {_id: 6, union: [1, 2, 3]},
        {_id: 7, union: [1, 2, 3]},
    ],
);

// Exercise the optimization of constant arguments.
let unionConstantArray = [3, 900, 3];
runTest(
    [{$project: {union: {$setUnion: ["$arr1", unionConstantArray]}}}],
    [
        {_id: 0, union: [1, 2, 3, 900]},
        {_id: 1, union: [1, 2, 3, 900]},
        {_id: 2, union: [1, 2, 3, 900]},
        {_id: 3, union: [3, 900]},
        {_id: 4, union: [1, 2, 3, 900]},
        {_id: 5, union: [2, 3, 900]},
        {_id: 6, union: [1, 2, 3, 900]},
        {_id: 7, union: [1, 2, 3, 900]},
    ],
);
runTest(
    [{$project: {union: {$setUnion: [unionConstantArray, "$arr2"]}}}],
    [
        {_id: 0, union: [2, 3, 4, 900]},
        {_id: 1, union: [3, 4, 5, 6, 900]},
        {_id: 2, union: [3, 900]},
        {_id: 3, union: [3, 4, 5, 6, 900]},
        {_id: 4, union: [2, 3, 900]},
        {_id: 5, union: [2, 3, 4, 900]},
        {_id: 6, union: [1, 2, 3, 900]},
        {_id: 7, union: [1, 2, 3, 900]},
    ],
);

runTest(
    [{$project: {intersection: {$setIntersection: ["$arr1", "$arr2"]}}}],
    [
        {_id: 0, intersection: [2, 3]},
        {_id: 1, intersection: []},
        {_id: 2, intersection: []},
        {_id: 3, intersection: []},
        {_id: 4, intersection: [2, 3]},
        {_id: 5, intersection: [2, 3]},
        {_id: 6, intersection: [1, 2, 3]},
        {_id: 7, intersection: [1, 2, 3]},
    ],
);

// Exercise the optimization of constant arguments.
let intersectionConstantArray = [1, 2, 1, 890, "long constant string"];
runTest(
    [{$project: {intersection: {$setIntersection: ["$arr1", intersectionConstantArray]}}}],
    [
        {_id: 0, intersection: [1, 2]},
        {_id: 1, intersection: [1, 2]},
        {_id: 2, intersection: [1, 2]},
        {_id: 3, intersection: []},
        {_id: 4, intersection: [1, 2]},
        {_id: 5, intersection: [2]},
        {_id: 6, intersection: [1, 2]},
        {_id: 7, intersection: [1, 2]},
    ],
);
runTest(
    [{$project: {intersection: {$setIntersection: [intersectionConstantArray, "$arr2"]}}}],
    [
        {_id: 0, intersection: [2]},
        {_id: 1, intersection: []},
        {_id: 2, intersection: []},
        {_id: 3, intersection: []},
        {_id: 4, intersection: [2]},
        {_id: 5, intersection: [2]},
        {_id: 6, intersection: [1, 2]},
        {_id: 7, intersection: [1, 2]},
    ],
);

runTest(
    [{$project: {difference: {$setDifference: ["$arr1", "$arr2"]}}}],
    [
        {_id: 0, difference: [1]},
        {_id: 1, difference: [1, 2, 3]},
        {_id: 2, difference: [1, 2, 3]},
        {_id: 3, difference: []},
        {_id: 4, difference: [1]},
        {_id: 5, difference: []},
        {_id: 6, difference: []},
        {_id: 7, difference: []},
    ],
);

runTest(
    [{$project: {difference: {$setDifference: ["$arr2", "$arr1"]}}}],
    [
        {_id: 0, difference: [4]},
        {_id: 1, difference: [4, 5, 6]},
        {_id: 2, difference: []},
        {_id: 3, difference: [4, 5, 6]},
        {_id: 4, difference: []},
        {_id: 5, difference: [4]},
        {_id: 6, difference: []},
        {_id: 7, difference: []},
    ],
);

// Exercise the optimization of constant arguments.
let differenceConstantArray = [3, 780];
runTest(
    [{$project: {difference: {$setDifference: ["$arr1", differenceConstantArray]}}}],
    [
        {_id: 0, difference: [1, 2]},
        {_id: 1, difference: [1, 2]},
        {_id: 2, difference: [1, 2]},
        {_id: 3, difference: []},
        {_id: 4, difference: [1, 2]},
        {_id: 5, difference: [2]},
        {_id: 6, difference: [1, 2]},
        {_id: 7, difference: [1, 2]},
    ],
);
runTest(
    [{$project: {difference: {$setDifference: [differenceConstantArray, "$arr2"]}}}],
    [
        {_id: 0, difference: [780]},
        {_id: 1, difference: [3, 780]},
        {_id: 2, difference: [3, 780]},
        {_id: 3, difference: [3, 780]},
        {_id: 4, difference: [780]},
        {_id: 5, difference: [780]},
        {_id: 6, difference: [780]},
        {_id: 7, difference: [780]},
    ],
);

runTest(
    [{$project: {equals: {$setEquals: ["$arr1", "$arr2"]}}}],
    [
        {_id: 0, equals: false},
        {_id: 1, equals: false},
        {_id: 2, equals: false},
        {_id: 3, equals: false},
        {_id: 4, equals: false},
        {_id: 5, equals: false},
        {_id: 6, equals: true},
        {_id: 7, equals: true},
    ],
);

// Exercise the optimization of constant arguments.
let equalConstantArray = [1, 2, 3, 2, 1];
runTest(
    [{$project: {equals: {$setEquals: ["$arr1", equalConstantArray]}}}],
    [
        {_id: 0, equals: true},
        {_id: 1, equals: true},
        {_id: 2, equals: true},
        {_id: 3, equals: false},
        {_id: 4, equals: true},
        {_id: 5, equals: false},
        {_id: 6, equals: true},
        {_id: 7, equals: true},
    ],
);
runTest(
    [{$project: {equals: {$setEquals: [equalConstantArray, "$arr2"]}}}],
    [
        {_id: 0, equals: false},
        {_id: 1, equals: false},
        {_id: 2, equals: false},
        {_id: 3, equals: false},
        {_id: 4, equals: false},
        {_id: 5, equals: false},
        {_id: 6, equals: true},
        {_id: 7, equals: true},
    ],
);

runTest(
    [{$project: {isSubset: {$setIsSubset: ["$arr1", "$arr2"]}}}],
    [
        {_id: 0, isSubset: false},
        {_id: 1, isSubset: false},
        {_id: 2, isSubset: false},
        {_id: 3, isSubset: true},
        {_id: 4, isSubset: false},
        {_id: 5, isSubset: true},
        {_id: 6, isSubset: true},
        {_id: 7, isSubset: true},
    ],
);

// Exercise the optimization of constant arguments.
runTest(
    [{$project: {isSubset: {$setIsSubset: ["$arr1", [2, 3, 4, "long constant string"]]}}}],
    [
        {_id: 0, isSubset: false},
        {_id: 1, isSubset: false},
        {_id: 2, isSubset: false},
        {_id: 3, isSubset: true},
        {_id: 4, isSubset: false},
        {_id: 5, isSubset: true},
        {_id: 6, isSubset: false},
        {_id: 7, isSubset: false},
    ],
);

// Exercise the optimization of constant arguments.
runTest(
    [{$project: {isSubset: {$setIsSubset: [[1, 3], "$arr2"]}}}],
    [
        {_id: 0, isSubset: false},
        {_id: 1, isSubset: false},
        {_id: 2, isSubset: false},
        {_id: 3, isSubset: false},
        {_id: 4, isSubset: false},
        {_id: 5, isSubset: false},
        {_id: 6, isSubset: true},
        {_id: 7, isSubset: true},
    ],
);

// No sets to union should produce an empty set for all records so we only check the first one.
assert.eq(coll.aggregate([{$project: {x: {$setUnion: []}}}]).toArray()[0]["x"], []);

// No sets to intersect should produce an empty set for all records so we only check the first one.
assert.eq(coll.aggregate([{$project: {x: {$setIntersection: []}}}]).toArray()[0]["x"], []);

const operators = [
    ["$setUnion", 17043],
    ["$setIntersection", 17047],
    ["$setDifference", [17048, 17049]],
    ["$setEquals", [17044, 7158100]],
    ["$setIsSubset", [17042, 17046]],
];
const badDocuments = [
    {arr1: "123", arr2: [1, 2, 3]},
    {arr1: [1, 2, 3], arr2: "123"},
    {arr1: "123", arr2: "123"},
];
for (const [operator, errorCodes] of operators) {
    for (const badDocument of badDocuments) {
        assert(coll.drop());
        assert.commandWorked(coll.insertOne(badDocument));
        assertErrorCode(coll, [{$project: {output: {[operator]: ["$arr1", "$arr2"]}}}], errorCodes);
    }
}

// Tests for null and missing values.
const nullMissingDocs = [
    {_id: 0, arr1: null, arr2: [1, 2, 3]},
    {_id: 1, arr1: [1, 2, 3], arr2: null},
    {_id: 2, arr1: null, arr2: null},
    {_id: 3, arr2: [1, 2, 3]},
    {_id: 4, arr1: [1, 2, 3]},
];

const nullResultOperators = ["$setUnion", "$setIntersection", "$setDifference"];
const errorCodeOperators = [
    ["$setEquals", 17044],
    ["$setIsSubset", [17042, 17046]],
];

for (const operator of nullResultOperators) {
    for (const doc of nullMissingDocs) {
        assert(coll.drop());
        assert.commandWorked(coll.insertOne(doc));
        const result = coll.aggregate([{$project: {output: {[operator]: ["$arr1", "$arr2"]}}}]).toArray();
        assert.eq(result[0].output, null, `Expected null result for operator ${operator} with document ${tojson(doc)}`);
    }
}

// ToDo: SERVER-107904. Remove check when 9.0 becomes last-lts
if (isMultiversion && !checkSbeCompletelyDisabled(db)) {
    jsTest.log.info("Skipping $setEquals and $setIsSubset tests on null or missing arrays for SBE.");
    quit();
}

for (const [operator, errorCodes] of errorCodeOperators) {
    for (const doc of nullMissingDocs) {
        assert(coll.drop());
        assert.commandWorked(coll.insertOne(doc));
        assertErrorCode(coll, [{$project: {output: {[operator]: ["$arr1", "$arr2"]}}}], errorCodes);
    }
}
