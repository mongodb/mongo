/**
 * Tests that the row store expression is skipped when there is an appropriate group or projection
 * above a columnscan stage.
 *
 * @tags: [
 *   # explain is not supported in transactions
 *   does_not_support_transactions,
 *   requires_pipeline_optimization,
 *   # Runs explain on an aggregate command which is only compatible with readConcern local.
 *   assumes_read_concern_unchanged,
 *   # explain will be different in a sharded collection
 *   assumes_unsharded_collection,
 *   # column store row store expression skipping is new in 6.2.
 *   requires_fcv_62,
 *   uses_column_store_index,
 * ]
 */
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For assertArrayEq.
load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.
// For areAllCollectionsClustered.
load("jstests/libs/clustered_collections/clustered_collection_util.js");

const columnstoreEnabled = checkSBEEnabled(
    db, ["featureFlagColumnstoreIndexes", "featureFlagSbeFull"], true /* checkAllNodes */);
if (!columnstoreEnabled) {
    jsTestLog("Skipping columnstore index test since the feature flag is not enabled.");
    return;
}

const indexedColl = db.column_scan_skip_row_store_projection_indexed;
const unindexedColl = db.column_scan_skip_row_store_projection_unindexed;

function setupCollections() {
    indexedColl.drop();
    unindexedColl.drop();
    assert.commandWorked(indexedColl.createIndex({"$**": "columnstore"}));

    const docs = [
        {_id: "a_number", a: 4},
        {_id: "a_subobject_c_not_null", a: {c: "hi"}},
        {_id: "a_subobject_c_null", a: {c: null}},
        {_id: "a_subobject_c_undefined", a: {c: undefined}},
        {_id: "no_a", b: 1},
        {_id: "a_and_b_nested", a: 2, b: {d: 1}},
        {_id: "a_nested_and_b_nested", a: {c: 5}, b: {d: {f: 2}}, e: 1},
    ];
    assert.commandWorked(indexedColl.insertMany(docs));
    assert.commandWorked(unindexedColl.insertMany(docs));
}

function test({agg, requiresRowStoreExpr, rowstoreFetches}) {
    // Check that columnstore index is used, and we skip the row store expression appropriately.
    const explainPlan = indexedColl.explain("queryPlanner").aggregate(agg);
    let sbeStages = ('queryPlanner' in explainPlan)
        // entirely SBE plan
        ? explainPlan.queryPlanner.winningPlan.slotBasedPlan.stages
        // SBE + classic plan
        : explainPlan.stages[0]["$cursor"].queryPlanner.winningPlan.slotBasedPlan.stages;
    assert(sbeStages.includes('columnscan'), `No columnscan in SBE stages: ${sbeStages}`);
    const nullRegex =
        /columnscan s.* ((s.*)|(none)) paths\[.*\] pathFilters\[.*\] rowStoreExpr\[\] @.* @.*/;
    const notNullRegex =
        /columnscan s.* ((s.*)|(none)) paths\[.*\] pathFilters\[.*\] rowStoreExpr\[.*, \n/;
    if (requiresRowStoreExpr) {
        assert(!nullRegex.test(sbeStages), `Don't expect null rowstoreExpr in ${sbeStages}`);
        assert(notNullRegex.test(sbeStages), `Expected non-null rowstoreExpr in ${sbeStages}`);
    } else {
        assert(nullRegex.test(sbeStages), `Expected null rowStoreExpr in ${sbeStages}`);
        assert(!notNullRegex.test(sbeStages), `Don't expect non-null rowStoreExpr in ${sbeStages}`);
    }
    // Check the expected number of row store fetches.
    const explainExec = indexedColl.explain("executionStats").aggregate(agg);
    const actualRowstoreFetches =
        parseInt(JSON.stringify(explainExec).split('"numRowStoreFetches":')[1].split(",")[0]);
    assert.eq(
        actualRowstoreFetches,
        rowstoreFetches,
        `Unexpected nubmer of row store fetches in ${JSON.stringify(explainExec, null, '\t')}`);

    // Check that results are identical with and without columnstore index.
    assertArrayEq({
        actual: indexedColl.aggregate(agg).toArray(),
        expected: unindexedColl.aggregate(agg).toArray()
    });
}

function runAllAggregations() {
    // $project only.  Requires row store expression regardless of nesting under the projected path.
    test({agg: [{$project: {_id: 0, a: 1}}], requiresRowStoreExpr: true, rowstoreFetches: 4});
    test({agg: [{$project: {_id: 0, b: 1}}], requiresRowStoreExpr: true, rowstoreFetches: 2});

    // $group only.
    // The 4 cases below provide the same coverage but illustrate when row store fetches are needed.
    test({
        agg: [{$group: {_id: null, a: {$push: "$a"}}}],
        requiresRowStoreExpr: false,
        rowstoreFetches: 4
    });
    test({
        agg: [{$group: {_id: null, b: {$push: "$b"}}}],
        requiresRowStoreExpr: false,
        rowstoreFetches: 2
    });
    test({
        agg: [{$group: {_id: null, e: {$push: "$e"}}}],
        requiresRowStoreExpr: false,
        rowstoreFetches: 0
    });
    test({
        agg: [{$group: {_id: "$_id", a: {$push: "$a"}, b: {$push: "$b"}}}],
        requiresRowStoreExpr: false,
        rowstoreFetches: 5
    });

    // $group and $project, including _id.
    test({
        agg: [{$project: {_id: 1, a: 1}}, {$group: {_id: "$_id", a: {$push: "$a"}}}],
        requiresRowStoreExpr: false,
        rowstoreFetches: 4
    });

    // The rowStoreExpr is needed to prevent the $group from seeing b.
    test({
        agg: [
            {$project: {_id: 1, a: 1}},
            {$group: {_id: "$_id", a: {$push: "$a"}, b: {$push: "$b"}}}
        ],
        requiresRowStoreExpr: true,
        rowstoreFetches: 4
    });

    // Same as above, but add another $group later that would be eligible for skipping the row store
    // expression.
    test({
        agg: [
            {$project: {_id: 1, a: 1}},
            {$group: {_id: "$_id", a: {$push: "$a"}, b: {$push: "$b"}}},
            {$project: {_id: 1, a: 1}},
            {$group: {_id: "$_id", a: {$push: "$a"}}}
        ],
        requiresRowStoreExpr: true,
        rowstoreFetches: 4
    });

    // $group and $project, excluding _id.
    // Because _id is projected out, the $group will aggregate all docs together.  The rowStoreExpr
    // must not be skipped or else $group will behave incorrectly.
    test({
        agg: [{$project: {_id: 0, a: 1}}, {$group: {_id: "$_id", a: {$push: "$a"}}}],
        requiresRowStoreExpr: true,
        rowstoreFetches: 4
    });

    // $match with a filter that can be pushed down.
    test({
        agg: [{$match: {a: 2}}, {$group: {_id: "$_id", b: {$push: "$b"}, a: {$push: "$a"}}}],
        requiresRowStoreExpr: false,
        rowstoreFetches: 1
    });

    // Nested paths.
    // The BrowserUsageByDistinctUserQuery that motivated this ticket is an example of this.
    test({
        agg: [{$match: {"a.c": 5}}, {$group: {_id: "$_id", b_d: {$push: "$b.d"}}}],
        requiresRowStoreExpr: false,
        rowstoreFetches: 1
    });

    // BrowserUsageByDistinctUserQuery from ColumnStoreIndex.yml in the genny repo.
    // $addFields is not implemented in SBE, so this will have an SBE plan + an agg pipeline.
    // This query does not match our documents, but the test checks for row store expression
    // elimination.
    test({
        agg: [
            {"$match": {"metadata.browser": {"$exists": true}}},
            {
                "$addFields":
                    {"browserName": {"$arrayElemAt": [{"$split": ["$metadata.browser", " "]}, 0]}}
            },
            {
                "$match": {
                    "browserName": {"$nin": [null, "", "null"]},
                    "created_at": {"$gte": ISODate("2020-03-10T01:17:41Z")}
                }
            },
            {
                "$group":
                    {"_id": {"__alias_0": "$browserName"}, "__alias_1": {"$addToSet": "$user_id"}}
            },
            {
                "$project":
                    {"_id": 0, "__alias_0": "$_id.__alias_0", "__alias_1": {"$size": "$__alias_1"}}
            },
            {"$project": {"label": "$__alias_0", "value": "$__alias_1", "_id": 0}},
            {"$limit": 5000}
        ],
        requiresRowStoreExpr: false,
        rowstoreFetches: 0
    });

    // Cases that may be improved by future work:

    // The limit below creates a Query Solution Node between the column scan and the group.
    // Our optimization is not clever enough to see that the limit QSN is irrelevant.
    test({
        agg: [{$limit: 100}, {$group: {_id: null, a: {$push: "$a"}}}],
        requiresRowStoreExpr: true,  // ideally this would be false
        rowstoreFetches: 4
    });

    // $match with a nested path filter than can be pushed down.
    // This fails to even use the column store index.  It should be able to in the future.
    assert.throws(() => {
        test({
            agg: [{$match: {"a.e": 1}}, {$group: {_id: "$_id", a: {$push: "$a"}}}],
            requiresRowStoreExpr: false,
            rowstoreFetches: 0
        });
    });
}

setupCollections();
runAllAggregations();
}());
