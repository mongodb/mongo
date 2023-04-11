/**
 * Testing of just the query layer's integration for columnar index.
 * @tags: [
 *   # Column store indexes are still under a feature flag.
 *   featureFlagColumnstoreIndexes,
 *   # Runs explain on an aggregate command which is only compatible with readConcern local.
 *   assumes_read_concern_unchanged,
 *   # Columnstore tests set server parameters to disable columnstore query planning heuristics -
 *   # 1) server parameters are stored in-memory only so are not transferred onto the recipient,
 *   # 2) server parameters may not be set in stepdown passthroughs because it is a command that may
 *   #      return different values after a failover
 *   tenant_migration_incompatible,
 *   does_not_support_stepdowns,
 *   not_allowed_with_security_token,
 *   uses_full_validation,
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

const coll = db.columnstore_index_correctness;

(function testColumnScanIsUsed() {
    coll.drop();
    coll.insert([{_id: 0, x: 42}]);  // the content doesn't matter for this test
    assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

    let explain = coll.find({}, {_id: 0, x: 1}).explain();
    assert(planHasStage(db, explain, "COLUMN_SCAN"),
           "Projection of existing column " + tojson(explain));

    explain = coll.find({}, {_id: 0, "x.y.z": 1, a: 1}).explain();
    assert(planHasStage(db, explain, "COLUMN_SCAN"),
           "Projection of non-existing columns " + tojson(explain));

    explain = coll.find({}, {x: 1, a: 1}).explain();
    assert(planHasStage(db, explain, "COLUMN_SCAN"),
           "Projection includes _id column " + tojson(explain));

    explain = coll.find({}, {_id: 0}).explain();
    assert(!planHasStage(db, explain, "COLUMN_SCAN"),
           "Exclusive projection cannot use column scan " + tojson(explain));
})();

// Run a query that tests SERVER-65494 (columnstore index shouldn't make us choke on empty paths).
(function testEmptyPaths() {
    const docs = [];
    for (let i = 0; i < 20; ++i) {
        docs.push({
            x: i,
            "": {"": i + 1, nonEmptyChild: i + 2},
            nonEmptyParent: {"": {"": i + 3, nonEmptyChild: i + 4}},
            nonEmptyArray: [{"": i + 5}]
        });
    }
    coll.drop();
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

    const projection = {_id: 0, x: 1};
    const probeRecord = docs[docs.length / 2];

    const filters = [
        {"": probeRecord[""]},
        {".": probeRecord[""][""]},
        {".nonEmptyChild": probeRecord[""]["nonEmptyChild"]},
        {"nonEmptyParent": probeRecord["nonEmptyParent"]},
        {"nonEmptyParent.": probeRecord["nonEmptyParent"][""]},
        {"nonEmptyParent..": probeRecord["nonEmptyParent"][""][""]},
        {"nonEmptyParent..nonEmptyChild": probeRecord["nonEmptyParent"][""]["nonEmptyChild"]},
        {"nonEmptyArray": probeRecord["nonEmptyArray"]},
        {"nonEmptyArray.": probeRecord["nonEmptyArray"][0][""]},
        {"nonEmptyArray.0": probeRecord["nonEmptyArray"][0]},
        {"nonEmptyArray.0.": probeRecord["nonEmptyArray"][0][""]},
    ];

    for (let filter of filters) {
        const trueResult = coll.find(filter, projection).hint({$natural: 1}).toArray();

        // Note that we intentionally don't hint or validate that the columnstore index is
        // being used. This is because we just care that the index doesn't cause these queries
        // to choke (we actually expect them to redirect to collection scan).

        const result = coll.find(filter, projection).toArray();
        assert.eq(result.length,
                  trueResult.length,
                  `Expected find to return ${trueResult.length} record(s) for filter:
      ${tojson(filter)}`);

        if (result.length > 0) {
            // Logically, the array cases should return results, but they don't with collscan.
            // We just care that the behavior matches so keep the validation less brittle here.
            assert(
                documentEq(result[0],
                           trueResult[0],
                           `Incorrect result for filter ${tojson(filter)}: ${tojson(result[0])}`));
        }
    }
})();

(function testColumnScanFindsAllDocumentsUsingDenseColumn() {
    // Check that column scan projections without filters will return all the documents, leveraging
    // dense columns internally regardless of whether _id is included or not. Internally, the dense
    // RowId Column will be scanned if the dense _id field is not included in the query. Scanning a
    // dense field column, which has a value present for every document in the collection, is
    // necessary to find documents missing all projected fields -- in the following queries
    // ultimately returning empty documents for such documents, rather than not at all.

    // Store documents where 'x' and 'y' fields are missing in some documents but present in other
    // documents. This should cause the dense _id or internal RowId columns to be used to identify
    // null field values.
    const docs = [
        {_id: 0, x: 1, y: "fee"},
        {_id: 1},
        {_id: 2, x: 1},
        {_id: 3, y: "fii"},
        {_id: 4, x: 1, y: "foo"},
        {_id: 5},
        {_id: 6, x: 1}
    ];
    coll.drop();
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

    const findDocsWithoutID = coll.find({}, {_id: 0, "x": 1, "y": 1}).toArray();
    assert.eq(findDocsWithoutID.length,
              docs.length,
              `Unexpected number of documents: ${tojson(findDocsWithoutID)}`);

    const findDocsWithID = coll.find({}, {_id: 1, "x": 1, "y": 1}).toArray();
    assert.eq(findDocsWithID.length,
              docs.length,
              `Unexpected number of documents: ${tojson(findDocsWithID)}`);
})();

(function testFieldsWithDotsAndDollars() {
    const doc = {
        _id: 1,
        "._id": 2,
        "": {"": 3},
        ".": 4,
        "a": {b: {c: 5}, "b.c": 6},
        "$": 7,
        "$a": 8,
        "$$a": {"$.$": 9}
    };
    coll.drop();
    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

    // Double check that the document was inserted correctly.
    const queryResult = coll.findOne();
    assert.docEq(queryResult, doc);

    // A projection on "a.b.c" should retrieve {"a": {"b": {"c": 5}}} but _not_ {"a": {"b.c": 6}}.
    const kProjection = {_id: 0, "a.b.c": 1};
    const explain = coll.find({}, kProjection).explain();
    assert(planHasStage(db, explain, "COLUMN_SCAN"),
           "Projection of existing column " + tojson(explain));

    const result = coll.findOne({}, kProjection);
    assert.docEq(result, {a: {b: {c: 5}}});

    // We cannot directly check the output of a column scan on most fields in the test document,
    // because there is no way to express projections on fields with empty names, internal dots, or
    // leading $s. For this test, we consider it sufficient that inserts to the column store do not
    // crash or corrupt data.
    const validationResult = assert.commandWorked(coll.validate({full: true}));
    assert(validationResult.valid, validationResult);
})();

// Multiple tests in this file use the same dataset. Intentionally not using _id as the unique
// identifier, to avoid getting IDHACK plans when we query by it.
const docs = [
    {num: 0},
    {num: 1, a: null},
    {num: 2, a: "scalar"},
    {num: 3, a: {}},
    {num: 4, a: {x: 1, b: "scalar"}},
    {num: 5, a: {b: {}}},
    {num: 6, a: {x: 1, b: {}}},
    {num: 7, a: {x: 1, b: {x: 1}}},
    {num: 8, a: {b: {c: "scalar"}}},
    {num: 9, a: {b: {c: null}}},
    {num: 10, a: {b: {c: [[1, 2], [{}], 2]}}},
    {num: 11, a: {x: 1, b: {x: 1, c: ["scalar"]}}},
    {num: 12, a: {x: 1, b: {c: {x: 1}}}},
    {num: 13, a: {b: []}},
    {num: 14, a: {b: [null]}},
    {num: 15, a: {b: ["scalar"]}},
    {num: 16, a: {b: [[]]}},
    {num: 17, a: {b: [1, {}, 2]}},
    {num: 18, a: {b: [[1, 2], [{}], 2]}},
    {num: 19, a: {x: 1, b: [[1, 2], [{}], 2]}},
    {num: 20, a: {b: [{c: "scalar"}]}},
    {num: 21, a: {b: [{c: "scalar"}, {c: "scalar2"}]}},
    {num: 22, a: {b: [{c: [[1, 2], [{}], 2]}]}},
    {num: 23, a: {b: [1, {c: "scalar"}, 2]}},
    {num: 24, a: {b: [1, {c: [[1, 2], [{}], 2]}, 2]}},
    {num: 25, a: {x: 1, b: [1, {c: [[1, 2], [{}], 2]}, 2]}},
    {num: 26, a: {b: [[1, 2], [{c: "scalar"}], 2]}},
    {num: 27, a: {b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}},
    {num: 28, a: {x: 1, b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}},
    {num: 29, a: []},
    {num: 30, a: [null]},
    {num: 31, a: ["scalar"]},
    {num: 32, a: [[]]},
    {num: 33, a: [{}]},
    {num: 34, a: [1, {}, 2]},
    {num: 35, a: [[1, 2], [{}], 2]},
    {num: 36, a: [{b: "scalar"}]},
    {num: 37, a: [{b: null}]},
    {num: 38, a: [1, {b: "scalar"}, 2]},
    {num: 39, a: [1, {b: []}, 2]},
    {num: 40, a: [1, {b: [null]}, 2]},
    {num: 41, a: [1, {b: ["scalar"]}, 2]},
    {num: 42, a: [1, {b: [[]]}, 2]},
    {num: 43, a: [{b: []}]},
    {num: 44, a: [{b: ["scalar"]}]},
    {num: 45, a: [{b: [[]]}]},
    {num: 46, a: [{b: {}}]},
    {num: 47, a: [{b: {c: "scalar"}}]},
    {num: 48, a: [{b: {c: [[1, 2], [{}], 2]}}]},
    {num: 49, a: [{b: {x: 1}}]},
    {num: 50, a: [{b: {x: 1, c: "scalar"}}]},
    {num: 51, a: [{b: [{c: "scalar"}]}]},
    {num: 52, a: [{b: [{c: ["scalar"]}]}]},
    {num: 53, a: [{b: [1, {c: ["scalar"]}, 2]}]},
    {num: 54, a: [{b: [{}]}]},
    {num: 55, a: [{b: [[1, 2], [{}], 2]}]},
    {num: 56, a: [{b: [[1, 2], [{c: "scalar"}], 2]}]},
    {num: 57, a: [{b: [[1, 2], [{c: ["scalar"]}], 2]}]},
    {num: 58, a: [1, {b: {}}, 2]},
    {num: 59, a: [1, {b: {c: "scalar"}}, 2]},
    {num: 60, a: [1, {b: {c: {x: 1}}}, 2]},
    {num: 61, a: [1, {b: {c: [1, {}, 2]}}, 2]},
    {num: 62, a: [1, {b: {x: 1}}, 2]},
    {num: 63, a: [1, {b: {x: 1, c: "scalar"}}, 2]},
    {num: 64, a: [1, {b: {x: 1, c: [[]]}}, 2]},
    {num: 65, a: [1, {b: {x: 1, c: [1, {}, 2]}}, 2]},
    {num: 66, a: [1, {b: [{}]}, 2]},
    {num: 67, a: [1, {b: [{c: "scalar"}]}, 2]},
    {num: 68, a: [1, {b: [{c: {x: 1}}]}, 2]},
    {num: 69, a: [1, {b: [{c: [1, {}, 2]}]}, 2]},
    {num: 70, a: [1, {b: [1, {}, 2]}, 2]},
    {num: 71, a: [1, {b: [1, {c: null}, 2]}, 2]},
    {num: 72, a: [1, {b: [1, {c: "scalar"}, 2]}, 2]},
    {num: 73, a: [1, {b: [1, {c: [1, {}, 2]}, 2]}, 2]},
    {num: 74, a: [1, {b: [[1, 2], [{}], 2]}, 2]},
    {num: 75, a: [1, {b: [[1, 2], [{c: "scalar"}], 2]}, 2]},
    {num: 76, a: [1, {b: [[1, 2], [{c: [1, {}, 2]}], 2]}, 2]},
    {num: 77, a: [[1, 2], [{b: "scalar"}], 2]},
    {num: 78, a: [[1, 2], [{b: {x: 1, c: "scalar"}}], 2]},
    {num: 79, a: [[1, 2], [{b: {x: 1, c: [1, {}, 2]}}], 2]},
    {num: 80, a: [[1, 2], [{b: []}], 2]},
    {num: 81, a: [[1, 2], [{b: [1, {c: "scalar"}, 2]}], 2]},
    {num: 82, a: [[1, 2], [{b: [[1, 2], [{c: "scalar"}], 2]}], 2]},
    {num: 83, a: [[1, 2], [{b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}], 2]},
    {num: 84, a: [{b: [{c: 1}, {}]}]},
    {num: 85, a: [{b: [{c: 1}, {d: 1}]}]},
    {num: 86, a: [{b: {c: 1}}, {b: {}}]},
    {num: 87, a: [{b: {c: 1}}, {b: {d: 1}}]},
    {num: 88, a: [{b: {c: 1}}, {}]},
    {num: 89, a: [{b: {c: 1}}, {b: null}]},
    {num: 90, a: [{b: {c: 1}}, {b: []}]},
    {num: 91, a: [{b: []}, {b: []}]},
    {num: 92, a: {b: [{c: [1, 2]}]}},
    {num: 93, a: {b: {c: [1, 2]}}},
    {num: 94, a: [[1, 2], [{b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}], 2]},
    {num: 95, a: [{m: 1, n: 2}, {m: 2, o: 1}]},
];

coll.drop();
let bulk = coll.initializeUnorderedBulkOp();
for (let doc of docs) {
    let insertObj = {};
    Object.assign(insertObj, doc);
    if (doc.num % 2 == 0) {
        insertObj.optionalField = "foo";
    }
    bulk.insert(insertObj);
}
bulk.execute();

assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

(function testProjectionOfIndependentPaths() {
    const kProjection = {_id: 0, "a.b.c": 1, num: 1, optionalField: 1};

    let explain = coll.find({}, kProjection).explain();
    assert(planHasStage(db, explain, "COLUMN_SCAN"),
           "Should have used column scan " + tojson(explain));

    let results = coll.find({}, kProjection).toArray();
    assert.eq(results.length, docs.length, "With no filter should have returned all docs");

    for (let res of results) {
        const trueResult = coll.find({num: res.num}, kProjection).hint({$natural: 1}).toArray()[0];
        const originalDoc = coll.findOne({num: res.num});
        assert.docEq(res, trueResult, "Mismatched projection of " + tojson(originalDoc));
    }
})();

// Run a similar query that projects multiple fields with a shared parent object.
(function testProjectionOfSiblingPaths() {
    const kSiblingProjection = {_id: 0, "a.m": 1, "a.n": 1, num: 1};

    let explain = coll.find({}, kSiblingProjection).explain();
    assert(planHasStage(db, explain, "COLUMN_SCAN"),
           "Should have used column scan " + tojson(explain));

    let results = coll.find({}, kSiblingProjection).toArray();
    assert.eq(results.length, docs.length, "With no filter should have returned all docs");

    for (let res of results) {
        const trueResult =
            coll.find({num: res.num}, kSiblingProjection).hint({$natural: 1}).toArray()[0];
        const originalDoc = coll.findOne({num: res.num});
        assert.eq(res, trueResult, "Mismatched projection of " + tojson(originalDoc));
    }
})();

// Run a query that tests the SERVER-67742 fix.
(function testPrefixPath() {
    const kPrefixProjection = {_id: 0, "a": 1, num: 1};

    // Have to use the index hint because SERVER-67264 blocks selection of CSI.
    let explain = coll.find({"a.m": 1}, kPrefixProjection).hint({"$**": "columnstore"}).explain();
    assert(planHasStage(db, explain, "COLUMN_SCAN"),
           "Should have used column scan " + tojson(explain));

    let results = coll.find({"a.m": 1}, kPrefixProjection).hint({"$**": "columnstore"}).toArray();
    let trueResults = coll.find({"a.m": 1}, kPrefixProjection).hint({$natural: 1}).toArray();
    assert.eq(results.length,
              trueResults.length,
              `Should have found the same number of docs but found:\n with index: ${
                  tojson(results)}\n without index: ${tojson(trueResults)}`);

    for (let res of results) {
        const trueResult =
            coll.find({num: res.num}, kPrefixProjection).hint({$natural: 1}).toArray()[0];
        const originalDoc = coll.findOne({num: res.num});
        assert.eq(res, trueResult, "Mismatched projection of " + tojson(originalDoc));
    }
})();

// Now test grouping semantics. Grouping limits the set of paths visible downstream which should
// allow column scan plans.
(function testGroup() {
    // Sanity check that we are comparing the plans we expect to be.
    let pipeline = [
        {$group: {_id: "$a.b.c", docs: {$push: "$num"}}},
        {$set: {docs: {$sortArray: {input: "$docs", sortBy: 1}}}}
    ];
    let naturalExplain = coll.explain().aggregate(pipeline, {hint: {$natural: 1}});
    assert(aggPlanHasStage(naturalExplain, "COLLSCAN"), naturalExplain);

    let nonHintedExplain = coll.explain().aggregate(pipeline);
    assert(aggPlanHasStage(nonHintedExplain, "COLUMN_SCAN"), nonHintedExplain);
    assert(!aggPlanHasStage(nonHintedExplain, "PROJECTION_DEFAULT"), nonHintedExplain);
    assert(!aggPlanHasStage(nonHintedExplain, "PROJECTION_SIMPLE"), nonHintedExplain);

    assert(resultsEq(coll.aggregate(pipeline, {hint: {$natural: 1}}).toArray(),
                     coll.aggregate(pipeline).toArray()),
           () => {
               print(`Results mismatch for $group query. Running resultsEq with verbose`);
               resultsEq(expectedResults, coll.aggregate(pipeline).toArray(), true);
           });

    // For readers who are taking on the massachistic task of trying to
    // verify that these results are in fact expected, the major expectations are that all arrays
    // are traversed and output as the "structure" EXCEPT if there's a doubly nested array without
    // any intervening path as in {a: [[{b: {c: 1}}]]}.
    const expectedResults = [
        {_id: "scalar", docs: [8]},
        {_id: ["scalar", "scalar2"], docs: [21]},
        {_id: ["scalar"], docs: [11, 20, 23, 47, 50, 59, 63]},
        {_id: [1, 2], docs: [93]},
        {_id: [1, []], docs: [90]},
        {_id: [1], docs: [86, 87, 88, 89]},
        {_id: [["scalar"]], docs: [51, 67, 72]},
        {_id: [[1, 2], [{}], 2], docs: [10]},
        {_id: [[1, 2]], docs: [92]},
        {_id: [[1, {}, 2]], docs: [61, 65]},
        {_id: [[1]], docs: [84, 85]},
        {_id: [[["scalar"]]], docs: [52, 53]},
        {_id: [[[1, 2], [{}], 2]], docs: [22, 24, 25, 48]},
        {_id: [[[1, {}, 2]]], docs: [69, 73]},
        {_id: [[[]]], docs: [64]},
        {_id: [[], []], docs: [91]},
        // Note "$a.b.c" does not descend into double (directly nested) arrays as in 42,45. Might
        // have expected [[[]]]. Similarly in 56,57, it does not find the "c" values "hidden" within
        // a directly-nested array.
        {_id: [[]], docs: [39, 40, 41, 42, 43, 44, 45, 54, 55, 56, 57, 66, 70, 74, 75, 76]},
        {_id: [[null]], docs: [71]},
        {_id: [[{x: 1}]], docs: [68]},
        {
            _id: [],
            docs: [
                13, 14, 15, 16, 17, 18, 19, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
                36, 37, 38, 46, 49, 58, 62, 77, 78, 79, 80, 81, 82, 83, 94, 95
            ]
        },
        {_id: [{x: 1}], docs: [60]},
        {_id: null, docs: [0, 1, 2, 3, 4, 5, 6, 7, 9]},
        {_id: {x: 1}, docs: [12]},
    ];

    assert(resultsEq(expectedResults, coll.aggregate(pipeline).toArray()), () => {
        print(`Results mismatch for $group query. Actual results: ${
            tojson(coll.aggregate(pipeline).toArray())} Running resultsEq with verbose`);
        resultsEq(expectedResults, coll.aggregate(pipeline).toArray(), true);
    });
})();

// Test count-like queries.
(function testCount() {
    let c = db.columnstore_index_count_correctness;
    c.drop();
    for (let i = 0; i < 5; ++i) {
        c.insert({a: 1, b: 4});
        c.insert({a: 2, b: 5});
        c.insert({a: 3, b: 6});
    }
    c.createIndex({"$**": "columnstore"});

    // Now test a few different count pipelines.
    let pipelines = [
        [{$match: {a: 1}}, {$count: "count"}],
        [{$count: "count"}],
        [{$match: {$or: [{a: 1}, {b: 5}]}}, {$count: "count"}],
        [{$match: {a: 1}}, {$group: {_id: null, count: {$sum: 1}}}]
    ];

    let expectedResults = [[{count: 5}], [{count: 15}], [{count: 10}], [{_id: null, count: 5}]];

    for (let i = 0; i < pipelines.length; ++i) {
        let pipeline = pipelines[i];

        let explain = c.explain().aggregate(pipeline);
        assert(aggPlanHasStage(explain, "COLUMN_SCAN"), explain);
        assert(!aggPlanHasStage(explain, "PROJECTION_DEFAULT"), explain);
        assert(!aggPlanHasStage(explain, "PROJECTION_SIMPLE"), explain);

        let actualResults = c.aggregate(pipeline).toArray();
        assert(resultsEq(expectedResults[i], actualResults), explain);
    }
})();

// Test column store queries with collations.
(function testCollation() {
    const c = db.columnstore_index_correctness_collation;
    c.drop();
    assert.commandWorked(c.createIndex({"$**": "columnstore"}));
    // Insert case sensitive values.
    assert.commandWorked(c.insert([{x: "hello"}, {x: "Hello"}, {x: "HELLO"}]));

    function runTest(collation, expectedMatches) {
        const explain = c.find({x: "hello"}, {_id: 0, x: 1}).collation(collation).explain();
        const columnScanPlanStages = getPlanStages(explain, "COLUMN_SCAN");
        assert.gte(columnScanPlanStages.length,
                   1,
                   `Could not find 'COLUMN_SCAN' stage: ${tojson(explain)}`);
        // Ensure that the filter actually got pushed down.
        assert(columnScanPlanStages[0].hasOwnProperty("filtersByPath") &&
                   columnScanPlanStages[0]["filtersByPath"].hasOwnProperty("x"),
               tojson(columnScanPlanStages));

        const actualMatches = c.find({x: "hello"}, {_id: 0, x: 1}).collation(collation).itcount();
        assert.eq(actualMatches,
                  expectedMatches,
                  `Expected to find ${expectedMatches} doc(s) using collation ${
                      tojson(collation)} but found ${actualMatches}`);
    }

    runTest({}, 1);                           // no collation
    runTest({locale: "en", strength: 3}, 1);  // case sensitive
    runTest({locale: "en", strength: 2}, 3);  // case insensitive
})();
})();
