/**
 * Testing of just the query layer's integration for columnar index when filters are used that
 * might be pushed down into the column scan stage.
 *
 * @tags: [
 *   # columnstore indexes are new in 6.2.
 *   requires_fcv_62,
 *   # Runs explain on an aggregate command which is only compatible with readConcern local.
 *   assumes_read_concern_unchanged,
 *   # TODO SERVER-66925 We could potentially need to resume an index build in the event of a
 *   # stepdown, which is not yet implemented.
 *   does_not_support_stepdowns,
 *   uses_column_store_index,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For "resultsEq."
load("jstests/libs/analyze_plan.js");         // For "planHasStage."
load("jstests/libs/sbe_explain_helpers.js");  // For getSbePlanStages.
load("jstests/libs/sbe_util.js");             // For "checkSBEEnabled.""

const columnstoreEnabled =
    checkSBEEnabled(db, ["featureFlagColumnstoreIndexes", "featureFlagSbeFull"]);
if (!columnstoreEnabled) {
    jsTestLog("Skipping columnstore index validation test since the feature flag is not enabled.");
    return;
}

const coll_filters = db.columnstore_index_per_path_filters;
function runPerPathFiltersTest({docs, query, projection, expected, testDescription}) {
    coll_filters.drop();
    coll_filters.insert(docs);
    assert.commandWorked(coll_filters.createIndex({"$**": "columnstore"}));

    // Order of the elements within the result and 'expected' is not significant for 'resultsEq' but
    // use small datasets to make the test failures more readable.
    const explain = coll_filters.find(query, projection).explain();
    const errMsg = " **TEST** " + testDescription + tojson(explain);
    const actual = coll_filters.find(query, projection).toArray();
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);
}
// Sanity check that without filters, the cursors on other columns are advanced correctly.
(function testPerPathFilters_SingleColumn_NoFilters() {
    const docs = [
        {n: 0, x: 42},
        {n: 1, y: 42},
        {n: 2, x: 42},
        {n: 3},
        {n: 4, x: 42, y: 42},
        {n: 5, x: 42},
    ];
    runPerPathFiltersTest({
        docs: docs,
        query: {},
        projection: {a: 1, _id: 0, n: 1, x: 1, y: 1},
        expected: docs,
        testDescription: "SingleColumn_NoFilters"
    });
})();

// Checks matching on top-level path that contains scalars and arrays.
(function testPerPathFilters_SingleColumn_TopLevelField_Match() {
    const docs = [
        {n: 0, x: 42},                              // vals: 42, arrInfo: <empty>
        {n: 1, x: [0, 42]},                         // vals: [0,42], arrInfo: "["
        {n: 2, x: [[0, 0, 0], 0, 42]},              // vals: [0,0,0,0,42], arrInfo: "[[|2]"
        {n: 3, x: [{y: 0}, {y: 0}, 42]},            // vals: [42], arrInfo: "[o1"
        {n: 4, x: [0, 0, {y: 0}, 42]},              // vals: [0,0,42], arrInfo: "[|1o"
        {n: 5, x: [[0, 0], {y: 0}, [{y: 0}], 42]},  // vals: [0,0,42], arrInfo: "[[|1]o[o]"
    ];
    // Projecting "_id" out and instead projecting as the first (in alphabetical order) column a
    // non-existing field helps to flush out bugs with incorrect lookup of the filtered columns
    // among all columns involved in the query.
    runPerPathFiltersTest({
        docs: docs,
        query: {x: 42},
        projection: {a: 1, _id: 0, n: 1},
        expected: [{n: 0}, {n: 1}, {n: 2}, {n: 3}, {n: 4}, {n: 5}],
        testDescription: "SingleColumn_TopLevelField_Match"
    });
})();

(function testPerPathFilters_SingleColumn_TopLevelField_NoMatch() {
    const docs = [
        {n: 0, x: {a: 0}},              // vals: <empty>, arrInfo: <empty>
        {n: 1, x: [[42, 0]]},           // vals: [42, 0], arrInfo: "[["
        {n: 2, x: [[42, 0, 0], 0, 0]},  // vals: [42,0,0,0,0], arrInfo: "[[|2]"
        {n: 3, x: [{y: 0}, [42, 0]]},   // vals: [42,0], arrInfo: "[o["
        {n: 4, x: [[42], {y: 0}]},      // vals: [42], arrInfo: "[[|]o"
        {n: 5, x: [[42, 42], [42]]},    // vals: [42,42,42], arrInfo: "[[|1]["
        {
            n: 6,
            x: [0, [42, [42, [42], 42], 42], [42]]
        },  // vals: [0,42,42,42,42,42], arrInfo: "[|[|[|[|]|]|]["
    ];
    // Projecting "_id" out and instead projecting as the first (in alphabetical order) column a
    // non-existing field helps to flush out bugs with incorrect lookup of the filtered columns
    // among all columns involved in the query.
    runPerPathFiltersTest({
        docs: docs,
        query: {x: 42},
        projection: {a: 1, _id: 0, n: 1},
        expected: [],
        testDescription: "SingleColumn_TopLevelField_NoMatch"
    });
})();

// Checks matching on sub-path that contains scalars and arrays.
(function testPerPathFilters_SingleColumn_SubField_Match() {
    const docs = [
        {_id: 0, x: {y: {z: 42}}},                           // vals: 42, arrInfo: <empty>
        {_id: 1, x: {y: {z: [0, 42]}}},                      // vals: [0,42], arrInfo: "{{["
        {_id: 2, x: {y: {z: [[0, 0], 42]}}},                 // vals: [0,0,42], arrInfo: "{{[[|1]"
        {_id: 3, x: {y: [{z: 0}, {z: 42}]}},                 // vals: [0,42], arrInfo: "{["
        {_id: 4, x: {y: [{z: {a: 0}}, {z: [[0, 0], 42]}]}},  // vals: [0,0,42], arrInfo: "{[o{[[|1]"
        {_id: 5, x: {y: [{a: 0}, {z: 0}, {a: 0}, {z: 42}]}},  // vals: [0,42], arrInfo: "{[1|+1"
        {
            _id: 6,
            x: [{y: {z: 0}}, {y: {z: [[0, 0]]}}, {y: {z: 42}}]
        },  // vals: [0,0,0,42], arrInfo: "[|{{[[|1]]"
        {
            _id: 7,
            x: [{y: {z: {a: 0}}}, {y: 0}, {y: {z: [0, 42]}}]
        },  // vals: [0,42], arrInfo: "[o+1{{["
        {
            _id: 8,
            x: [{y: [{z: {a: 0}}, {z: [0]}, {a: 0}, {z: [0, 42]}]}]
        },                                                // vals: [0,0,42], arrInfo: "[{[o{[|]+1{["
        {_id: 9, x: [{y: [[{z: 0}], {z: 42}]}]},          // vals: [0,42], arrInfo: "[{[[|]"
        {_id: 10, x: [[{y: [{z: 0}]}], {y: {z: [42]}}]},  // vals: [0,42], arrInfo: "[[{[|]]{{["
        {_id: 11, x: [{y: {z: [0]}}, {y: {z: [42]}}]},    // vals: [0,42], arrInfo: "[{{[|]{{["
    ];
    let expected = [];
    for (let i = 0; i < docs.length; i++) {
        expected.push({_id: i});
    }
    runPerPathFiltersTest({
        docs: docs,
        query: {"x.y.z": 42},
        projection: {_id: 1},
        expected: expected,
        testDescription: "SingleColumn_SubField_Match"
    });
})();

// Checks matching on sub-path that contains scalars and arrays.
(function testPerPathFilters_SingleColumn_SubField_NoMatch() {
    const docs = [
        {_id: 0, x: {y: {z: {a: 0}}}},               // vals: <empty>, arrInfo: <empty>
        {_id: 1, x: {y: {z: [0, [42]]}}},            // vals: [0,42], arrInfo: "{{[|["
        {_id: 2, x: {y: [{z: 0}, [{z: 42}]]}},       // vals: [0,42], arrInfo: "{[|["
        {_id: 3, x: [{y: {z: 0}}, [{y: {z: 42}}]]},  // vals: [42], arrInfo: "[1["
    ];
    runPerPathFiltersTest({
        docs: docs,
        query: {"x.y.z": 42},
        projection: {_id: 1},
        expected: [],
        testDescription: "SingleColumn_SubField_NoMatch"
    });
})();

// Check matching of empty arrays which are treated as values.
// NB: expressions for comparing to whole non-empty arrays aren't split into per-path filters.
(function testPerPathFilters_EmptyArray() {
    const docs = [
        {_id: 0, x: {y: []}},
        {_id: 1, x: {y: [0, []]}},
        {_id: 2, x: [{y: 0}, {y: []}]},
        {_id: 3, x: {y: [0, [0, []]]}},
    ];
    runPerPathFiltersTest({
        docs: docs,
        query: {"x.y": 42},
        projection: {_id: 1},
        expected: [],  //[{_id: 0}, {_id: 1}, {_id: 2}],
        testDescription: "SingleColumn_EmptyArray"
    });
})();

// Check matching of empty objects which are treated as values.
// NB: expressions for comparing to whole non-empty objects aren't split into per-path filters.
(function testPerPathFilters_EmptyObject() {
    const docs = [
        {_id: 0, x: {y: {}}},
        {_id: 1, x: {y: [0, {}]}},
        {_id: 2, x: [{y: 0}, {y: {}}]},
        {_id: 3, x: {y: [0, [0, {}]]}},
    ];
    runPerPathFiltersTest({
        docs: docs,
        query: {"x.y": {}},
        projection: {_id: 1},
        expected: [{_id: 0}, {_id: 1}, {_id: 2}],
        testDescription: "SingleColumn_EmptyArray"
    });
})();

// Check that a single filtered column correctly handles matching, no-matching and missing values
// when moving the cursor.
(function testPerPathFilters_SingleColumnMatchNoMatchMissingValues() {
    const docs = [
        {_id: 0, x: 42},
        {_id: 1, x: 0},
        {_id: 2, x: 42},
        {_id: 3, no_x: 0},
        {_id: 4, x: 42},
        {_id: 5, x: 0},
        {_id: 6, no_x: 0},
        {_id: 7, x: 42},
        {_id: 8, no_x: 0},
        {_id: 9, x: 0},
        {_id: 10, x: 42},
    ];
    runPerPathFiltersTest({
        docs: docs,
        query: {x: 42},
        projection: {_id: 1},
        expected: [{_id: 0}, {_id: 2}, {_id: 4}, {_id: 7}, {_id: 10}],
        testDescription: "SingleColumnMatchNoMatchMissingValues"
    });
})();

// Check zig-zagging of two filters. We cannot assert through a JS test that the cursors for the
// filtered columns are advanced as described here in the comments, but the test attempts to
// exercise various match/no-match/missing combinations of values across columns.
(function testPerPathFilters_TwoColumns() {
    const docs = [
        {_id: 0, x: 0, y: 0},        // start by iterating x
        {_id: 1, x: 42, y: 42},      // seek into y and match! - continue iterating on x
        {_id: 2, x: 42, no_y: 0},    // seeking into y skips to n:3
        {_id: 3, x: 42, y: 0},       // iterating on y
        {_id: 4, x: 42, y: 42},      // seek into x and match! - continue iterating on y
        {_id: 5, no_x: 0, y: 42},    // seek into x skips to n:6
        {_id: 6, x: 42, y: 0},       // seek into y but no match - iterate on y
        {_id: 7, x: 0, y: 42},       // seek into x but no match - iterate on x
        {_id: 8, no_x: 0, no_y: 0},  // skipped by x
        {_id: 9, x: 42, y: 42},      // seek into y and match!
    ];
    // Adding into the projection specification non-existent fields doesn't change the output but
    // helps to flush out bugs with incorrect indexing of filtered paths among all others.
    runPerPathFiltersTest({
        docs: docs,
        query: {x: 42, y: 42},
        projection: {_id: 1, a: 1, xa: 1, xb: 1, ya: 1},
        expected: [{_id: 1}, {_id: 4}, {_id: 9}],
        testDescription: "TwoColumns"
    });
})();

// Check zig-zagging of three filters.
(function testPerPathFilters_ThreeColumns() {
    const docs = [
        {_id: 0, x: 0, y: 42, z: 42},     // x
        {_id: 1, x: 42, y: 42, z: 0},     // x->y->z
        {_id: 2, x: 0, y: 42, z: 42},     // z->x
        {_id: 3, x: 0, y: 42, z: 42},     // x
        {_id: 4, x: 42, no_y: 0, z: 42},  // x->y
        {_id: 5, x: 42, y: 0, z: 42},     // y
        {_id: 6, x: 42, y: 42, z: 42},    // match! ->y
        {_id: 7, x: 42, y: 42, z: 42},    // match! ->y
        {_id: 8, no_x: 0, y: 42, z: 42},  // y->z->x
        {_id: 9, x: 42, y: 42, no_z: 0},  // x
        {_id: 10, x: 42, y: 42, z: 42},   // match!
    ];
    // Adding into the projection specification non-existent fields doesn't change the output but
    // helps to flush out bugs with incorrect indexing of filtered paths among all others.
    runPerPathFiltersTest({
        docs: docs,
        query: {x: 42, y: 42, z: 42},
        projection: {_id: 1, a: 1, b: 1, xa: 1, xb: 1, ya: 1, za: 1},
        expected: [{_id: 6}, {_id: 7}, {_id: 10}],
        testDescription: "ThreeColumns"
    });
})();

// Check projection of filtered columns.
(function testPerPathFilters_ProjectFilteredColumn() {
    const docs = [
        {_id: 0, x: {y: 42}},
        {_id: 1, x: {y: 42, z: 0}},
        {_id: 2, x: [0, {y: 42}, {y: 0}, {z: 0}]},
    ];
    runPerPathFiltersTest({
        docs: docs,
        query: {"x.y": 42},
        projection: {_id: 1, "x.y": 1},
        expected: [{_id: 0, x: {y: 42}}, {_id: 1, x: {y: 42}}, {_id: 2, x: [{y: 42}, {y: 0}, {}]}],
        testDescription: "ProjectFilteredColumn"
    });
})();

// Check correctness when have both per-path and residual filters.
(function testPerPathFilters_PerPathAndResidualFilters() {
    const docs = [
        {_id: 0, x: 42, no_y: 0},
        {_id: 1, x: 42, y: 0},
        {_id: 2, x: 0, no_y: 0},
        {_id: 3, x: 0, y: 0},
        {_id: 4, no_x: 0, no_y: 0},
        {_id: 5, x: 42, no_y: 0},
    ];
    runPerPathFiltersTest({
        docs: docs,
        query: {x: 42, y: {$exists: false}},  // {$exists: false} causes the residual filter
        projection: {_id: 1},
        expected: [{_id: 0}, {_id: 5}],
        testDescription: "PerPathAndResidualFilters"
    });
})();

// Check translation of MQL comparison match expressions.
(function testPerPathFilters_SupportedMatchExpressions_Comparison() {
    const docs = [
        {_id: 0, x: NumberInt(5)},
        {_id: 1, x: 0},
        {_id: 2, x: null},
        {_id: 3, no_x: 0},
        {_id: 4, x: NumberInt(15)},
    ];

    coll_filters.drop();
    coll_filters.insert(docs);
    assert.commandWorked(coll_filters.createIndex({"$**": "columnstore"}));

    let expected = [];
    let actual = [];
    let errMsg = "";

    // MatchExpression::LT
    actual = coll_filters.find({x: {$lt: 15}}, {_id: 1}).toArray();
    expected = [{_id: 0}, {_id: 1}];
    errMsg = "SupportedMatchExpressions: $lt";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);

    // MatchExpression::GT
    actual = coll_filters.find({x: {$gt: 5}}, {_id: 1}).toArray();
    expected = [{_id: 4}];
    errMsg = "SupportedMatchExpressions: $gt";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);

    // MatchExpression::LTE
    actual = coll_filters.find({x: {$lte: 15}}, {_id: 1}).toArray();
    expected = [{_id: 0}, {_id: 1}, {_id: 4}];
    errMsg = "SupportedMatchExpressions: $lte";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);

    // MatchExpression::GTE
    actual = coll_filters.find({x: {$gte: 5}}, {_id: 1}).toArray();
    expected = [{_id: 0}, {_id: 4}];
    errMsg = "SupportedMatchExpressions: $gte";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);
})();

// Check translation of MQL bitwise match expressions.
(function testPerPathFilters_SupportedMatchExpressions_Bitwise() {
    const docs = [
        {_id: 0, x: NumberInt(1 + 0 * 2 + 1 * 4 + 0 * 8)},
        {_id: 1, x: 0},
        {_id: 2, x: null},
        {_id: 3, no_x: 0},
        {_id: 4, x: NumberInt(0 + 1 * 2 + 1 * 4 + 1 * 8)},
        {_id: 5, x: NumberInt(0 + 1 * 2 + 1 * 4 + 0 * 8)},
    ];

    coll_filters.drop();
    coll_filters.insert(docs);
    assert.commandWorked(coll_filters.createIndex({"$**": "columnstore"}));

    let expected = [];
    let actual = [];
    let errMsg = "";

    // MatchExpression::BITS_ALL_SET
    actual = coll_filters.find({x: {$bitsAllSet: [1, 2]}}, {_id: 1}).toArray();
    expected = [{_id: 4}, {_id: 5}];
    errMsg = "SupportedMatchExpressions: $bitsAllSet";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);

    // MatchExpression::BITS_ALL_CLEAR
    actual = coll_filters.find({x: {$bitsAllClear: [1, 3]}}, {_id: 1}).toArray();
    expected = [{_id: 0}, {_id: 1}];
    errMsg = "SupportedMatchExpressions: $bitsAllClear";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);

    // MatchExpression::BITS_ANY_SET
    actual = coll_filters.find({x: {$bitsAnySet: [0, 3]}}, {_id: 1}).toArray();
    expected = [{_id: 0}, {_id: 4}];
    errMsg = "SupportedMatchExpressions: $bitsAnySet";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);

    // MatchExpression::BITS_ANY_CLEAR
    actual = coll_filters.find({x: {$bitsAnyClear: [2, 3]}}, {_id: 1}).toArray();
    expected = [{_id: 0}, {_id: 1}, {_id: 5}];
    errMsg = "SupportedMatchExpressions: $bitsAnyClear";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);
})();

// Check translation of MQL $exists, $not and $ne match expressions.
// NB: per SERVER-68743 the queries in this test won't be using per-path filters yet, but eventually
// they should.
(function testPerPathFilters_SupportedMatchExpressions_Not() {
    const docs = [
        {_id: 0, x: 42},
        {_id: 1, x: 0},
        {_id: 2, x: null},
        {_id: 3, no_x: 0},
        {_id: 4, x: []},
        {_id: 5, x: {}},
    ];

    coll_filters.drop();
    coll_filters.insert(docs);
    assert.commandWorked(coll_filters.createIndex({"$**": "columnstore"}));

    let expected = [];
    let actual = [];
    let errMsg = "";

    actual = coll_filters.find({x: {$ne: null}}, {_id: 1}).toArray();
    expected = [{_id: 0}, {_id: 1}, {_id: 4}, {_id: 5}];
    errMsg = "SupportedMatchExpressions: $ne";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);

    actual = coll_filters.find({x: {$not: {$eq: null}}}, {_id: 1}).toArray();
    expected = [{_id: 0}, {_id: 1}, {_id: 4}, {_id: 5}];
    errMsg = "SupportedMatchExpressions: $not + $eq";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);

    actual = coll_filters.find({x: {$exists: true}}, {_id: 1}).toArray();
    expected = [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 4}, {_id: 5}];
    errMsg = "SupportedMatchExpressions: $exists";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);
})();

// Check translation of other MQL match expressions.
(function testPerPathFilters_SupportedMatchExpressions_Oddballs() {
    const docs = [
        {_id: 0, x: NumberInt(7 * 3 + 4)},
        {_id: 1, x: NumberInt(7 * 9 + 2)},
        {_id: 2, x: "find me"},
        {_id: 3, x: "find them"},
    ];

    coll_filters.drop();
    coll_filters.insert(docs);
    assert.commandWorked(coll_filters.createIndex({"$**": "columnstore"}));

    let expected = [];
    let actual = [];
    let errMsg = "";

    actual = coll_filters.find({x: {$mod: [7, 2]}}, {_id: 1}).toArray();
    expected = [{_id: 1}];
    errMsg = "SupportedMatchExpressions: $mod";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);

    actual = coll_filters.find({x: {$regex: /me/}}, {_id: 1}).toArray();
    expected = [{_id: 2}];
    errMsg = "SupportedMatchExpressions: $regex";
    assert(resultsEq(actual, expected),
           `actual=${tojson(actual)}, expected=${tojson(expected)}${errMsg}`);
})();

// Check degenerate case with no paths.
(function testPerPathFilters_NoPathsProjected() {
    const docs = [
        {_id: 0, x: 42},
        {_id: 1, x: 0},
        {_id: 2, no_x: 42},
    ];
    runPerPathFiltersTest({
        docs: docs,
        query: {x: 42},
        projection: {_id: 0, a: {$literal: 1}},
        expected: [{a: 1}],
        testDescription: "NoPathsProjected"
    });
    runPerPathFiltersTest({
        docs: docs,
        query: {},
        projection: {_id: 0, a: {$literal: 1}},
        expected: [{a: 1}, {a: 1}, {a: 1}],
        testDescription: "NoPathsProjected (and no filter)"
    });
})();

// While using columnar indexes doesn't guarantee a specific field ordering in the result objects,
// we still try to provide a stable experience to the user, so we output "_id" first and other
// fields in alphabetically ascending order.
(function testPerPathFilters_FieldOrder() {
    const docs = [{z: 42, a: 42, _id: 42, _a: 42}];

    coll_filters.drop();
    coll_filters.insert(docs);
    assert.commandWorked(coll_filters.createIndex({"$**": "columnstore"}));

    let res = tojson(coll_filters.find({}, {_a: 1, a: 1, z: 1}).toArray()[0]);
    let expected = '{ "_id" : 42, "_a" : 42, "a" : 42, "z" : 42 }';
    assert(res == expected, `actual: ${res} != expected: ${expected} in **TEST** field order 1`);

    // Having a filter on a path that is also being projected, should not affect the order.
    res = tojson(coll_filters.find({z: 42}, {_a: 1, a: 1, z: 1}).toArray()[0]);
    expected = '{ "_id" : 42, "_a" : 42, "a" : 42, "z" : 42 }';
    assert(res == expected, `actual: ${res} != expected: ${expected} in **TEST** field order 2`);

    // Omitting the "_id" field should not affect the order.
    res = tojson(coll_filters.find({a: 42}, {_id: 0, _a: 1, z: 1}).toArray()[0]);
    expected = '{ "_a" : 42, "z" : 42 }';
    assert(res == expected, `actual: ${res} != expected: ${expected} in **TEST** field order 3`);
})();

// Sanity test that per-column filtering is meeting the efficiency expectations.
(function testPerPathFilters_ExecutionStats() {
    coll_filters.drop();

    const docsCount = 1000;
    const expectedToMatchCount = 10;
    for (let i = 0; i < docsCount; i++) {
        coll_filters.insert({_id: i, x: i % 2, y: i % (docsCount / expectedToMatchCount)});
    }
    assert.commandWorked(coll_filters.createIndex({"$**": "columnstore"}));

    assert.eq(coll_filters.find({x: 1, y: 1}, {_id: 1, x: 1}).toArray().length,
              expectedToMatchCount);
    const explain = coll_filters.find({x: 1, y: 1}, {_id: 1, x: 1}).explain("executionStats");

    const columnScanStages = getSbePlanStages(explain, "COLUMN_SCAN");
    assert.gt(columnScanStages.length, 0, `Could not find 'COLUMN_SCAN' stage: ${tojson(explain)}`);

    if (columnScanStages.length > 1) {
        // The test is being run in sharded environment and the state per shard would depend on
        // how the data gets distributed.
        jsTestLog("Skipping testPerPathFilters_ExecutionStats in sharded environment.");
        return;
    }

    const columns = columnScanStages[0].columns;
    assertArrayEq({
        actual: columnScanStages[0].paths,
        expected: ["_id", "x", "y"],
        extraErrorMsg: 'Paths used by column scan stage'
    });
    assert.eq(Object.keys(columns).length, 3, `Used colums ${columns}`);

    const _id = columns._id;
    const x = columns.x;
    const y = columns.y;

    assert(_id.usedInOutput, "_id column should be used in output");
    assert(x.usedInOutput, "x column should be used in output");
    assert(!y.usedInOutput, "y column should be used only for filtering");

    // When there are no missing fields, the number of "next" calls in zig-zag search algorithm is
    // equal to the number of documents docsCount (NB: in non-filtered search the number of "next"
    // calls is close to k*docsCount where k is the number of paths).
    assert.eq(_id.numNexts, 0, 'Should not iterate on non-filtered column');
    assert.eq(x.numNexts + y.numNexts, docsCount, 'Total count of "next" calls');

    // The columns with higher selectivity should be preferred by the zig-zag search for driving the
    // "next" calls. Due to the regularity of data in this test (if "y" matched "x" would match as
    // well), after the first match "y" column completely takes over.
    assert.gt(y.numNexts, 0.9 * docsCount, 'high selectivity should drive "next" calls');

    // We seek into each column to set up the cursors, after that seeks into _id should only happen
    // on full match, and seeks into x or y should only happen on partial matches.
    assert.eq(_id.numSeeks, 1 + expectedToMatchCount, "Seeks into the _id column");
    assert.lt(x.numSeeks + y.numSeeks,
              2 * expectedToMatchCount,
              "Number of seeks in filtered columns should be small");
})();
})();
