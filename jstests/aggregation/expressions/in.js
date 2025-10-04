// SERVER-6146 introduced the $in expression to aggregation. In this file, we test the functionality
// and error cases of the expression.
// @tags: [
//   assumes_no_implicit_collection_creation_after_drop,
// ]
import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const caseSensitive = {
    locale: "en",
    strength: 3,
};

const caseInsensitive = {
    locale: "en_US",
    strength: 2,
};
let coll = db.in;
coll.drop();

// To call testExpressionWithIntersection, the options must have a array1 and array2 field.
function testExpressionWithIntersection(options) {
    coll.drop();
    let pipeline = {
        $project: {
            included: {
                $in: ["$elementField", {$setIntersection: [{$literal: options.array1}, {$literal: options.array2}]}],
            },
        },
    };
    assert.commandWorked(coll.insert({elementField: options.element}));
    let res = coll.aggregate(pipeline).toArray();
    testExpressionEquivalence(res, options);
    testQueryFormEquivalence(res, options);
}

function testExpression(options) {
    coll.drop();
    testExpressionInternal(options);
}

function testExpressionHashIndex(options) {
    coll.drop();
    assert.commandWorked(coll.createIndex({elementField: "hashed"}));
    testExpressionInternal(options);
}

function testExpressionCollectionCollation(options, collationSpec) {
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: collationSpec}));
    testExpressionInternal(options);
}

function testExpressionInternal(options) {
    let pipeline = {$project: {included: {$in: ["$elementField", {$literal: options.array}]}}};
    assert.commandWorked(coll.insert({elementField: options.element}));
    let res = coll.aggregate(pipeline).toArray();
    testExpressionEquivalence(res, options);
    testQueryFormEquivalence(res, options);
}

function testExpressionEquivalence(res, options) {
    assert.eq(res.length, 1);
    assert.eq(res[0].included, options.elementIsIncluded);
}

function testQueryFormEquivalence(res, options) {
    if (options.queryFormShouldBeEquivalent) {
        let query = {elementField: {$in: options.array}};
        res = coll.find(query).toArray();

        if (options.elementIsIncluded) {
            assert.eq(res.length, 1);
        } else {
            assert.eq(res.length, 0);
        }
    }
}

testExpression({element: 1, array: [1, 2, 3], elementIsIncluded: true, queryFormShouldBeEquivalent: true});

testExpression({
    element: "A",
    array: ["a", "A", "a"],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true,
});

testExpression({
    element: {a: 1},
    array: [{b: 1}, 2],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: true,
});

/* ------------------------ Nested Objects Tests ------------------------ */

testExpression({element: {a: 1}, array: [{a: 1}], elementIsIncluded: true, queryFormShouldBeEquivalent: true});

testExpression({
    element: [1, 2],
    array: [[2, 1]],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: true,
});

testExpression({element: [1, 2], array: [[1, 2]], elementIsIncluded: true, queryFormShouldBeEquivalent: true});

/* ------------------------ Duplicated Elements Tests ------------------------ */

// Test $in with duplicated target element.
testExpression({
    element: 7,
    array: [3, 5, 7, 7, 9],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true,
});

// Test $in with other element within array duplicated.
testExpression({
    element: 7,
    array: [3, 5, 7, 9, 9],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true,
});

/* ------------------------ Unsorted Array Tests ------------------------ */

// Test $in on unsorted array.
testExpression({
    element: 7,
    array: [3, 10, 5, 7, 8, 9],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true,
});

// Test matching $in on unsorted array with duplicates.
testExpression({
    element: 7,
    array: [7, 10, 7, 10, 2, 5, 3, 7],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true,
});

// Test non-matching $in on unsorted array with duplicates.
testExpression({
    element: 8,
    array: [10, 7, 2, 5, 3],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: true,
});

/* ------------------------ Collator Tests ------------------------ */

// Test $in with success due to collation on source collection.
testExpressionCollectionCollation(
    {
        element: "abcd",
        array: ["aBcD", "ABCD"],
        elementIsIncluded: true,
        queryFormShouldBeEquivalent: true,
    },
    caseInsensitive,
);

// Test $in with failure with collation
testExpressionCollectionCollation(
    {
        element: "abcd",
        array: ["aBcD", "ABCD"],
        elementIsIncluded: false,
        queryFormShouldBeEquivalent: true,
    },
    caseSensitive,
);

testExpressionWithIntersection({
    element: 1,
    array1: [1, 2, 3],
    array2: [2, 3, 4],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: false,
});

testExpressionWithIntersection({
    element: 2,
    array1: [1, 2, 3],
    array2: [2, 3, 4],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: false,
});

testExpressionWithIntersection({
    element: 1,
    array1: [1, 2, 3],
    array2: [4, 5, 6],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: false,
});

testExpressionWithIntersection({
    element: 1,
    array1: [1, 2, 3],
    array2: [],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: false,
});

testExpressionWithIntersection({
    element: 1,
    array1: [],
    array2: [4, 5, 6],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: false,
});

/* ------------------------ Mismatched Types Tests ------------------------ */

testExpression({
    element: 1,
    array: [1, "a", 32.04],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true,
});

testExpression({
    element: 1,
    array: [2, "a", 32.04],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: true,
});

/* ------------------------ Miscellaneous Tests ------------------------ */

// Test $in with a source collection that has a hash index on the relevant field.
testExpressionHashIndex({
    element: 5,
    array: [10, 7, 2, 5, 3],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true,
});

testExpression({element: 1, array: [], elementIsIncluded: false, queryFormShouldBeEquivalent: true});

// Aggregation's $in has parity with query's $in except with regexes matching string values and
// equality semantics with array values.

testExpression({
    element: "abc",
    array: [/a/, /b/, /c/],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: false,
});

testExpression({
    element: /a/,
    array: ["a", "b", "c"],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: false,
});

testExpression({element: [], array: [1, 2, 3], elementIsIncluded: false, queryFormShouldBeEquivalent: false});

testExpression({element: [1], array: [1, 2, 3], elementIsIncluded: false, queryFormShouldBeEquivalent: false});

testExpression({
    element: [1, 2],
    array: [1, 2, 3],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: false,
});

coll.drop();
coll.insert({});

/* ------------------------ Assertion Failure Tests ------------------------ */

let pipeline = {$project: {included: {$in: [[1, 2], 1]}}};
assertErrorCode(coll, pipeline, 40081, "$in requires an array as a second argument");

pipeline = {
    $project: {included: {$in: [1, null]}},
};
assertErrorCode(coll, pipeline, 40081, "$in requires an array as a second argument");

pipeline = {
    $project: {included: {$in: [1, "$notAField"]}},
};
assertErrorCode(coll, pipeline, 40081, "$in requires an array as a second argument");

pipeline = {
    $project: {included: {$in: null}},
};
assertErrorCode(coll, pipeline, 16020, "$in requires two arguments");

pipeline = {
    $project: {included: {$in: [1]}},
};
assertErrorCode(coll, pipeline, 16020, "$in requires two arguments");

pipeline = {
    $project: {included: {$in: []}},
};
assertErrorCode(coll, pipeline, 16020, "$in requires two arguments");

pipeline = {
    $project: {included: {$in: [1, 2, 3]}},
};
assertErrorCode(coll, pipeline, 16020, "$in requires two arguments");
