/**
 * Test column stores indexes that use a "columnstoreProjection" or "prefix.$**" notation to limit
 * indexed data to a subset of the document namespace.
 * @tags: [
 *   requires_fcv_63,
 *   # Runs explain on an aggregate command which is only compatible with readConcern local.
 *   assumes_read_concern_unchanged,
 *   # Columnstore tests set server parameters to disable columnstore query planning heuristics -
 *   # 1) server parameters are stored in-memory only so are not transferred onto the recipient,
 *   # 2) server parameters may not be set in stepdown passthroughs because it is a command that may
 *   #      return different values after a failover
 *   tenant_migration_incompatible,
 *   does_not_support_stepdowns,
 *   not_allowed_with_security_token,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/analyze_plan.js");         // For "planHasStage."
load("jstests/aggregation/extras/utils.js");  // For "resultsEq."
load("jstests/libs/columnstore_util.js");     // For "setUpServerForColumnStoreIndexTest."

if (!setUpServerForColumnStoreIndexTest(db)) {
    return;
}

//
// Test the intended use case of the column store index: queries that project several inpedendent
// paths that can be efficiently scanned the way that a column in a tabular database can be scanned.
//

function runTestWithDocsAndIndexes(collName, testFn, docs, ...indexes) {
    const coll = db[`${jsTestName()}_${collName}`];
    coll.drop();
    assert.commandWorked(coll.insert(docs));
    for (let {keys, options} of indexes) {
        assert.commandWorked(coll.createIndex(keys, options));
    }

    testFn(coll);
}

const docsWithNestedPaths = [
    {num: 0},
    {num: 1, a: {b: {c: "scalar"}}, str: "a"},
    {num: 2, a: {b: {c: [[1, 2], [{}], 2]}}},
    {num: 3, a: {x: 1, b: {x: 1, c: ["scalar"]}}},
    {num: 4, a: {x: 1, b: {c: {x: 1}}}, str: ["b", "c"]},
    {num: 5, a: {b: [{c: "scalar"}, {c: "scalar2"}]}},
    {num: 6, a: {b: [{c: [[1, 2], [{}], 2]}]}},
    {num: 7, a: [{b: {c: "scalar"}}]},
    {num: 8, a: [{b: {x: 1, c: "scalar"}}], str: ["d", {}, "e"]},
    {num: 9, a: [{b: [1, {c: ["scalar"]}, 2]}]},
    {num: 10, a: [{b: [{}]}]},
    {num: 11, a: [1, {b: {c: {x: 1}}}, 2], str: []},
    {num: 12, a: [1, {b: {c: [1, {}, 2]}}, 2]},
    {num: 13, a: [1, {b: [{c: "scalar"}]}, 2], str: {x: 1, y: 1}},
    {num: 14, a: {b: [{c: [1, 2]}]}},
    {num: 15, a: {b: {c: [1, 2]}}, str: ["f", {x: 1, y: 1}, "g"]},
    {num: 16, a: [[1, 2], [{b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}], 2]},
];
const testWithProjection = projection => coll => {
    let explain = coll.find({}, projection).explain();
    assert(planHasStage(db, explain, "COLUMN_SCAN"), explain);

    let results = coll.find({}, projection).toArray();
    let expectedresults = coll.find({}, projection).hint({$natural: 1}).toArray();
    assert.sameMembers(results, expectedresults, coll.getName());
};

runTestWithDocsAndIndexes("independent_paths_1",
                          testWithProjection({_id: 0, "a.b.c": 1, num: 1, str: 1}),
                          docsWithNestedPaths,
                          {keys: {"$**": "columnstore"}});

runTestWithDocsAndIndexes(
    "independent_paths_2",
    testWithProjection({_id: 0, "a.b.c": 1, num: 1, str: 1}),
    docsWithNestedPaths,
    {keys: {"$**": "columnstore"}, options: {columnstoreProjection: {a: 1, num: 1, str: 1}}});

runTestWithDocsAndIndexes(
    "independent_paths_3",
    testWithProjection({_id: 0, "a.b.c": 1, num: 1, str: 1}),
    docsWithNestedPaths,
    {keys: {"$**": "columnstore"}, options: {columnstoreProjection: {"a.b.c": 1, num: 1, str: 1}}});

// Test an exclusion projection that should still be eligible for a column scan.
runTestWithDocsAndIndexes(
    "independent_paths_4",
    testWithProjection({_id: 0, "a.b.c": 1, num: 1, str: 1}),
    docsWithNestedPaths,
    {keys: {"$**": "columnstore"}, options: {columnstoreProjection: {"a.x": 0}}});

// Test that execution can use an eligible column store index even when other column store indexes
// exist that are ineligible. One of these three indexes is always ineligible, because it does not
// include the "str" field, and one is ineligible in passthroughs with collections that are sharded
// on "_id" (as in some of the sharded collection passthrough suites).
runTestWithDocsAndIndexes(
    "independent_paths_5",
    testWithProjection({_id: 0, "a.b.c": 1, num: 1, str: 1}),
    docsWithNestedPaths,
    {keys: {"$**": "columnstore"}, options: {name: "csi1", columnstoreProjection: {"a.b.c": 1}}},
    {
        keys: {"$**": "columnstore"},
        options: {name: "csi2", columnstoreProjection: {_id: 0, "a": 1, num: 1, str: 1}}
    },
    {
        keys: {"$**": "columnstore"},
        options: {name: "csi3", columnstoreProjection: {"a.b.c": 1, num: 1, str: 1}}
    });

//
// Explicitly verify that the planner does not choose a column store index whose projection is
// incompatible with the query projection.
//

const testIneligibleProjection = projection => coll => {
    let explain = coll.find({}, projection).explain();
    assert(!planHasStage(db, explain, "COLUMN_SCAN"), explain);
};

runTestWithDocsAndIndexes(
    "ineligible_1",
    testIneligibleProjection({_id: 0, "a.b.c": 1, num: 1, str: 1}),
    [{_id: 1}],
    {keys: {"$**": "columnstore"}, options: {columnstoreProjection: {"a.b.c": 1, str: 1}}});

runTestWithDocsAndIndexes(
    "ineligible_2",
    testIneligibleProjection({_id: 0, "a.b.c": 1, num: 1, str: 1}),
    [{_id: 1}],
    {keys: {"$**": "columnstore"}, options: {columnstoreProjection: {"a.b.c": 0}}});

// Test a column store index that uses "prefix.$**" notation.
runTestWithDocsAndIndexes("ineligible_3",
                          testIneligibleProjection({_id: 0, "a.b.c": 1, num: 1, str: 1}),
                          [{_id: 1}],
                          {keys: {"a.$**": "columnstore"}});

// Test a collection with multiple ineligible indexes.
runTestWithDocsAndIndexes(
    "ineligible_4",
    testIneligibleProjection({_id: 0, "a.b.c": 1, num: 1, str: 1}),
    [{_id: 1}],
    {
        keys: {"$**": "columnstore"},
        options: {name: "csi1", columnstoreProjection: {"a.b.c": 1, str: 1}}
    },
    {keys: {"$**": "columnstore"}, options: {name: "csi2", columnstoreProjection: {"a.b.c": 0}}},
    {keys: {"a.$**": "columnstore"}, options: {name: "csi3"}});

//
// Test a projection on multiple fields with a shared parent object.
//

const docsWithSiblingPaths = [
    {num: 1, a: {m: 1, n: 2}},
    {num: 2, a: [{m: 1, n: 2}, {m: 2, o: 1}]},
    {num: 3, a: [{m: 1, n: 2}, {m: [3, 4], o: 1}]},
];

runTestWithDocsAndIndexes("sibling_paths_1",
                          testWithProjection({_id: 0, "a.m": 1, "a.n": 1}),
                          docsWithSiblingPaths,
                          {keys: {"$**": "columnstore"}});

runTestWithDocsAndIndexes("sibling_paths_2",
                          testWithProjection({_id: 0, "a.m": 1, "a.n": 1}),
                          docsWithSiblingPaths,
                          {keys: {"$**": "columnstore"}, columnstoreProjection: {a: 1}});

// Test an exclusion projection that should still be eligible for a column scan.
runTestWithDocsAndIndexes("sibling_paths_3",
                          testWithProjection({_id: 0, "a.m": 1, "a.n": 1}),
                          docsWithSiblingPaths,
                          {keys: {"$**": "columnstore"}, columnstoreProjection: {"a.b": 0}});

// Test that execution can use an eligible column store index even when other column store indexes
// exist that are ineligible. One of these three indexes is always ineligible, because it does not
// include the "a.m" field, and one is ineligible in passthroughs with collections that are sharded
// on "_id" (as in some of the sharded collection passthrough suites).
runTestWithDocsAndIndexes(
    "sibling_paths_4",
    testWithProjection({_id: 0, "a.m": 1, "a.n": 1}),
    docsWithSiblingPaths,
    {keys: {"$**": "columnstore"}, options: {name: "csi1", columnstoreProjection: {"a.m": 0}}},
    {keys: {"a.$**": "columnstore"}, options: {name: "csi2"}},
    {keys: {"$**": "columnstore"}, options: {name: "csi3", columnstoreProjection: {"a": 1}}});

//
// Repeat the above projection on fields with a shared parent but without explicitly verifying that
// the plan uses a column scan, so that we can use a "prefix.$**"-style column scan index. We cannot
// expect this index to get used in sharded collection passthrough suites, because it is not
// eligible for scatter-gather queries on collections that have "_id" in their shard key.
//
const testCorrectnessWithProjection = projection => coll => {
    let results = coll.find({}, projection).toArray();
    let expectedresults = coll.find({}, projection).hint({$natural: 1}).toArray();
    assert.sameMembers(results, expectedresults);
};

runTestWithDocsAndIndexes("sibling_paths_5",
                          testCorrectnessWithProjection({_id: 0, "a.m": 1, "a.n": 1}),
                          docsWithSiblingPaths,
                          {keys: {"a.$**": "columnstore"}});

// Note that this test does not drop any of its test collections or indexes, so that they will be
// available to follow-on index validation tests.
})();
