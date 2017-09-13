// SERVER-6146 introduced the $in expression to aggregation. In this file, we test the functionality
// and error cases of the expression.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

(function() {
    "use strict";

    var coll = db.in ;
    coll.drop();

    function testExpression(options) {
        var pipeline = {$project: {included: {$in: ["$elementField", {$literal: options.array}]}}};

        coll.drop();
        assert.writeOK(coll.insert({elementField: options.element}));
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

    testExpression({
        element: {a: 1},
        array: [{a: 1}],
        elementIsIncluded: true,
        queryFormShouldBeEquivalent: true
    });

    testExpression({
        element: [1, 2],
        array: [[2, 1]],
        elementIsIncluded: false,
        queryFormShouldBeEquivalent: true
    });

    testExpression({
        element: [1, 2],
        array: [[1, 2]],
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

    testExpression({
        element: [],
        array: [1, 2, 3],
        elementIsIncluded: false,
        queryFormShouldBeEquivalent: false
    });

    testExpression({
        element: [1],
        array: [1, 2, 3],
        elementIsIncluded: false,
        queryFormShouldBeEquivalent: false
    });

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

    pipeline = {$project: {included: {$in: [1, null]}}};
    assertErrorCode(coll, pipeline, 40081, "$in requires an array as a second argument");

    pipeline = {$project: {included: {$in: [1, "$notAField"]}}};
    assertErrorCode(coll, pipeline, 40081, "$in requires an array as a second argument");

    pipeline = {$project: {included: {$in: null}}};
    assertErrorCode(coll, pipeline, 16020, "$in requires two arguments");

    pipeline = {$project: {included: {$in: [1]}}};
    assertErrorCode(coll, pipeline, 16020, "$in requires two arguments");

    pipeline = {$project: {included: {$in: []}}};
    assertErrorCode(coll, pipeline, 16020, "$in requires two arguments");

    pipeline = {$project: {included: {$in: [1, 2, 3]}}};
    assertErrorCode(coll, pipeline, 16020, "$in requires two arguments");
}());
