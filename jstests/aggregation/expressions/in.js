// SERVER-6146 introduced the $in expression to aggregation. In this file, we test the functionality
// and error cases of the expression.
// @tags: [assumes_no_implicit_collection_creation_after_drop]
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

(function() {
"use strict";

const caseInsensitive = {
    locale: "en_US",
    strength: 2
};
var coll = db.in;
coll.drop();

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
    var pipeline = {$project: {included: {$in: ["$elementField", {$literal: options.array}]}}};
    assert.commandWorked(coll.insert({elementField: options.element}));
    var res = coll.aggregate(pipeline).toArray();
    assert.eq(res.length, 1);
    assert.eq(res[0].included, options.elementIsIncluded);

    if (options.queryFormShouldBeEquivalent) {
        var query = {elementField: {$in: options.array}};
        res = coll.find(query).toArray();

        if (options.elementIsIncluded) {
            assert.eq(res.length, 1);
        } else {
            assert.eq(res.length, 0);
        }
    }
}

testExpression(
    {element: 1, array: [1, 2, 3], elementIsIncluded: true, queryFormShouldBeEquivalent: true});

testExpression({
    element: "A",
    array: ["a", "A", "a"],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true
});

testExpression({
    element: {a: 1},
    array: [{b: 1}, 2],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: true
});

testExpression(
    {element: {a: 1}, array: [{a: 1}], elementIsIncluded: true, queryFormShouldBeEquivalent: true});

testExpression({
    element: [1, 2],
    array: [[2, 1]],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: true
});

testExpression(
    {element: [1, 2], array: [[1, 2]], elementIsIncluded: true, queryFormShouldBeEquivalent: true});

// Test $in with duplicated target element.
testExpression({
    element: 7,
    array: [3, 5, 7, 7, 9],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true
});

// Test $in with other element within array duplicated.
testExpression({
    element: 7,
    array: [3, 5, 7, 9, 9],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true
});

// Test $in on unsorted array.
testExpression({
    element: 7,
    array: [3, 10, 5, 7, 8, 9],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true
});

// Test matching $in on unsorted array with duplicates.
testExpression({
    element: 7,
    array: [7, 10, 7, 10, 2, 5, 3, 7],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true
});

// Test non-matching $in on unsorted array with duplicates.
testExpression({
    element: 8,
    array: [10, 7, 2, 5, 3],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: true
});

// Test $in with success due to collation on source collection.
testExpressionCollectionCollation({
    element: "abcd",
    array: ["aBcD", "ABCD"],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true
},
                                  caseInsensitive);

// Test $in with a source collection that has a hash index on the relevant field.
testExpressionHashIndex({
    element: 5,
    array: [10, 7, 2, 5, 3],
    elementIsIncluded: true,
    queryFormShouldBeEquivalent: true
});

testExpression(
    {element: 1, array: [], elementIsIncluded: false, queryFormShouldBeEquivalent: true});

// Aggregation's $in has parity with query's $in except with regexes matching string values and
// equality semantics with array values.

testExpression({
    element: "abc",
    array: [/a/, /b/, /c/],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: false
});

testExpression({
    element: /a/,
    array: ["a", "b", "c"],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: false
});

testExpression(
    {element: [], array: [1, 2, 3], elementIsIncluded: false, queryFormShouldBeEquivalent: false});

testExpression(
    {element: [1], array: [1, 2, 3], elementIsIncluded: false, queryFormShouldBeEquivalent: false});

testExpression({
    element: [1, 2],
    array: [1, 2, 3],
    elementIsIncluded: false,
    queryFormShouldBeEquivalent: false
});

coll.drop();
coll.insert({});

var pipeline = {$project: {included: {$in: [[1, 2], 1]}}};
assertErrorCode(coll, pipeline, 40081, "$in requires an array as a second argument");

pipeline = {
    $project: {included: {$in: [1, null]}}
};
assertErrorCode(coll, pipeline, 40081, "$in requires an array as a second argument");

pipeline = {
    $project: {included: {$in: [1, "$notAField"]}}
};
assertErrorCode(coll, pipeline, 40081, "$in requires an array as a second argument");

pipeline = {
    $project: {included: {$in: null}}
};
assertErrorCode(coll, pipeline, 16020, "$in requires two arguments");

pipeline = {
    $project: {included: {$in: [1]}}
};
assertErrorCode(coll, pipeline, 16020, "$in requires two arguments");

pipeline = {
    $project: {included: {$in: []}}
};
assertErrorCode(coll, pipeline, 16020, "$in requires two arguments");

pipeline = {
    $project: {included: {$in: [1, 2, 3]}}
};
assertErrorCode(coll, pipeline, 16020, "$in requires two arguments");
}());
