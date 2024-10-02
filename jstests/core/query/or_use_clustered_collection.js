/**
 * Verifies that $or queries on clustered collections produce plans with IXSCAN and
 * CLUSTERED_IXSCAN stages when possible.
 * @tags: [
 *   requires_fcv_71,
 *  # Explain for the aggregate command cannot run within a multi-document transaction.
 *  does_not_support_transactions,
 *  # Refusing to run a test that issues an aggregation command with explain because it may return
 *  # incomplete results if interrupted by a stepdown.
 *  does_not_support_stepdowns
 * ]
 */

import {
    getAggPlanStages,
    getPlanStage,
    getPlanStages,
    getWinningPlan
} from "jstests/libs/analyze_plan.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";

const coll = db.or_use_clustered_collection;
assertDropCollection(db, coll.getName());

const isSbeGroupEnabled = checkSbeRestrictedOrFullyEnabled(db);

// Create a clustered collection and create indexes.
assert.commandWorked(
    db.createCollection(coll.getName(), {clusteredIndex: {key: {_id: 1}, unique: true}}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({c: 1}));
assert.commandWorked(coll.createIndex({b: "text"}));

// Insert documents, and store them to be used later in the test.
const docs = [];
const textFields = ["foo", "one", "two", "three", "four", "foo", "foo", "seven", "eight", "nine"];
const numDocs = textFields.length;
for (let i = 0; i < numDocs; i++) {
    docs.push({b: textFields[i], a: i, _id: i, c: i * 2, d: [{e: i * 2}, {g: i / 2}], noIndex: i});
}
assert.commandWorked(coll.insertMany(docs));

function haveShardMergeStage(winningPlan, stage = "SHARD_MERGE") {
    let shardMergeStage = getPlanStages(winningPlan, stage);
    return shardMergeStage.length > 0;
}

function assertCorrectResults({query, expectedDocIds, projection, limit, skip}) {
    // Test different find queries. With and without a sort, and with and without a projection.
    let results = query.toArray();
    let expectedResults = [];
    //  Create the document set that we expect.
    if (skip) {
        // Confirm we only skipped 1 document.
        assert.eq(results.length, expectedDocIds.length - 1);
        // Remove the document that was skipped.
        expectedDocIds = expectedDocIds.filter(id => results.some(el => el["_id"] == id));
    }
    expectedDocIds.forEach(id => projection
                               ? expectedResults.push({"_id": docs[id]["_id"], "a": docs[id]["a"]})
                               : expectedResults.push(docs[id]));
    if (limit) {
        assert.eq(results.length, 2);
        assert.neq(results[0]["_id"], results[1]["_id"]);
        for (let i = 0; i < results.length; ++i) {
            let doc = expectedResults.filter(r => r["_id"] == results[i]["_id"]);
            assert.eq(1, doc.length);
            assert.docEq(doc[0], results[i]);
        }
        return;
    }

    assert.sameMembers(results, expectedResults);
}

// $or query which uses a clustered collection scan plan for one branch and secondary index plan for
// the other, and returns no matching documents.
assertCorrectResults({query: coll.find({$or: [{_id: 123}, {a: 11}]}), expectedDocIds: []});

//$or query which uses a clustered collection scan plan and secondary index plan, and each predicate
// matches some of the documents.
assertCorrectResults(
    {query: coll.find({$or: [{_id: 9}, {a: {$lte: 3}}]}), expectedDocIds: [0, 1, 2, 3, 9]});

// $or query which uses a clustered collection scan plan and secondary index plan, and some
// documents match both predicates.
assertCorrectResults(
    {query: coll.find({$or: [{_id: {$lt: 2}}, {a: {$lte: 3}}]}), expectedDocIds: [0, 1, 2, 3]});

//  $or query that uses two clustered collection scan plans.
assertCorrectResults(
    {query: coll.find({$or: [{_id: {$lt: 2}}, {_id: {$gt: 8}}]}), expectedDocIds: [0, 1, 9]});

// $or query that uses two secondary index scan plans.
assertCorrectResults(
    {query: coll.find({$or: [{a: {$lt: 2}}, {a: {$gt: 8}}]}), expectedDocIds: [0, 1, 9]});

function validateQueryPlan({query, expectedStageCount, expectedDocIds, noFetchWithCount}) {
    // TODO SERVER-77601 add coll.find(query).sort({_id: 1}) to 'queries'.
    const testCases = [
        {
            explainQuery: coll.explain().find(query).finish(),
            additionalStages: {},
            actualQuery: coll.find(query)
        },
        {
            explainQuery: coll.explain().find(query, {_id: 1, a: 1}).limit(2).finish(),
            additionalStages: {"LIMIT": 1, "PROJECTION_SIMPLE": 1},
            actualQuery: coll.find(query, {_id: 1, a: 1}).limit(2),
        },
        {
            explainQuery: coll.explain().find(query).limit(2).finish(),
            additionalStages: {"LIMIT": 1},
            actualQuery: coll.find(query).limit(2),
        },
        {
            explainQuery: coll.explain().find(query).skip(1).finish(),
            additionalStages: {"SKIP": 1},
            actualQuery: coll.find(query).skip(1),
        },
        {
            explainQuery: coll.explain().aggregate([{$match: query}, {$project: {_id: 1, a: 1}}]),
            additionalStages: {"PROJECTION_SIMPLE": 1},
            actualQuery: coll.aggregate([{$match: query}, {$project: {_id: 1, a: 1}}]),
            aggregate: true,
        },
        {
            explainQuery: coll.explain().aggregate(
                [{$match: query}, {$group: {_id: null, count: {$sum: 1}}}]),
            additionalStages: {"GROUP": 1},
            actualQuery: coll.aggregate([{$match: query}, {$group: {_id: null, count: {$sum: 1}}}]),
            aggregate: true
        },
        {
            explainQuery: coll.explain().find(query).count(),
            additionalStages: {"COUNT": 1},
            actualQuery: coll.find(query).count(),
        }
    ];

    testCases.forEach(test => {
        const explain = test.explainQuery;

        // If there is a 'SHARD_MERGE' stage or 'shards', then we should expect more than our
        // 'expectedStageCount', since each stage will appear for each shard.
        const shardMergeStage = getPlanStage(explain, "SHARD_MERGE");
        const shards = "shards" in explain;

        // There won't be a 'FETCH' stage if we have a 'COUNT' or 'GROUP' stage with just index scan
        // plans.
        const count = test.additionalStages.hasOwnProperty('COUNT');
        const fetch = expectedStageCount.hasOwnProperty('FETCH');
        const group = test.additionalStages.hasOwnProperty('GROUP');
        const skip = test.additionalStages.hasOwnProperty('SKIP');
        if (noFetchWithCount && (count || group) && fetch) {
            expectedStageCount["FETCH"] = 0;
        }

        // Classic engine doesn't have a GROUP stage like SBE for $group.
        if (group && !isSbeGroupEnabled) {
            test.additionalStages["GROUP"] = 0;
        }

        // The SKIP stage isn't forwarded to the shards when the query is transformed.
        if (skip && (shardMergeStage || shards)) {
            test.additionalStages["SKIP"] = 0;
        }

        // Validate all the stages appear the correct number of times in the winning plan.
        const expectedStages = Object.assign({}, expectedStageCount, test.additionalStages);
        for (let stage in expectedStages) {
            let planStages =
                test.aggregate ? getAggPlanStages(explain, stage) : getPlanStages(explain, stage);
            assert(planStages, tojson(explain));
            if (shardMergeStage || shards) {
                assert.gte(planStages.length,
                           expectedStages[stage],
                           "Expected " + stage + " to appear, but got plan: " + tojson(explain));
            } else {
                assert.eq(planStages.length,
                          expectedStages[stage],
                          "Expected " + stage + " to appear, but got plan: " + tojson(explain));
            }
        }

        const projection = test.additionalStages.hasOwnProperty('PROJECTION_SIMPLE');
        const limit = test.additionalStages.hasOwnProperty('LIMIT');
        if (count || group) {
            // If we have GROUP stage we are in an aggregation pipeline.
            let results = group ? test.actualQuery.toArray()[0]["count"] : test.actualQuery;
            assert.eq(expectedDocIds.length,
                      results,
                      "Expected " + expectedDocIds.length.toString() + " number of docs, but got " +
                          tojson(test.actualQuery));
        } else {
            assertCorrectResults({
                query: test.actualQuery,
                expectedDocIds: expectedDocIds,
                projection: projection,
                limit: limit,
                skip: skip,
            });
        }
    });
}

// Validates that we use an OR stage with the correct plans for each child branch.
function validateQueryOR({query, expectedStageCount, expectedDocIds, noFetchWithCount}) {
    expectedStageCount["OR"] = 1;
    validateQueryPlan({
        query: query,
        expectedStageCount: expectedStageCount,
        expectedDocIds: expectedDocIds,
        noFetchWithCount: noFetchWithCount
    });
}

// $or with a CLUSTERED_IXSCAN stage and a IXSCAN stage.
validateQueryOR({
    query: {$or: [{_id: {$lt: 2}}, {a: 5}]},
    expectedStageCount: {"CLUSTERED_IXSCAN": 1, "IXSCAN": 1, "FETCH": 1},
    expectedDocIds: [0, 1, 5],
});

validateQueryOR({
    query: {$or: [{_id: 5}, {a: 6}]},
    expectedStageCount: {"CLUSTERED_IXSCAN": 1, "IXSCAN": 1, "FETCH": 1},
    expectedDocIds: [5, 6],
});

// $or with two IXSCAN stages.
validateQueryOR({
    query: {$or: [{c: {$gte: 10}}, {a: 0}]},
    expectedStageCount: {"IXSCAN": 2, "FETCH": 1},
    expectedDocIds: [0, 5, 6, 7, 8, 9],
    // This is an optimization for IXSCAN for count queries that does not exist for plans with
    // clustered indexes.
    noFetchWithCount: true
});

// $or with 2 CLUSTERED_IXSCAN stages.
validateQueryOR({
    query: {$or: [{_id: {$lt: 1}}, {_id: {$gt: 8}}]},
    expectedStageCount: {"CLUSTERED_IXSCAN": 2},
    expectedDocIds: [0, 9]
});

validateQueryOR({
    query: {$or: [{_id: {$gt: 5}}, {_id: 8}]},
    expectedStageCount: {"CLUSTERED_IXSCAN": 2},
    expectedDocIds: [6, 7, 8, 9]
});

// $or with many children branches that are either IXSCAN or CLUSTERED_IXSCAN stages. Note that we
//  expect our IXSCAN nodes to be optimized down to one stage.
validateQueryOR({
    query: {$or: [{_id: {$gt: 5}}, {_id: 8}, {a: 1}, {a: 1}, {a: {$gte: 8}}]},
    expectedStageCount: {"CLUSTERED_IXSCAN": 2, "IXSCAN": 1},
    expectedDocIds: [1, 6, 7, 8, 9]
});

// $or with many children branches that are either IXSCAN or CLUSTERED_IXSCAN stages.
validateQueryOR({
    query: {$or: [{_id: {$gt: 7}}, {_id: 8}, {a: 1}, {a: {$gte: 8}}, {c: {$lt: 10}}]},
    expectedStageCount: {"CLUSTERED_IXSCAN": 2, "IXSCAN": 2},
    expectedDocIds: [0, 1, 2, 3, 4, 8, 9]
});

// $or query where the branch of the clustered collection scan is not a leaf node.
validateQueryOR({
    query: {$or: [{a: 1}, {$and: [{_id: {$gt: 7}}, {_id: {$lt: 10}}]}]},
    expectedStageCount: {"CLUSTERED_IXSCAN": 1, "IXSCAN": 1, "FETCH": 1},
    expectedDocIds: [1, 8, 9]
});

// $or inside an $and should not change, and still use a FETCH with an IXSCAN.
validateQueryPlan({
    query: {$and: [{a: {$gte: 8}}, {$or: [{_id: 2}, {c: {$gt: 10}}]}]},
    expectedStageCount: {"FETCH": 1, "IXSCAN": 1, "OR": 0},
    expectedDocIds: [8, 9],
});

// $or that can't use the clustered collection nor another index should still fallback to COLLSCAN.
validateQueryPlan({
    query: {$or: [{noIndex: 3}, {_id: 1}]},
    expectedStageCount: {"COLLSCAN": 1, "OR": 0},
    expectedDocIds: [1, 3],
});

validateQueryPlan({
    query: {$or: [{noIndex: 3}, {a: 1}]},
    expectedStageCount: {"COLLSCAN": 1, "OR": 0},
    expectedDocIds: [1, 3],
});

//$or inside an $elemMatch that is not indexed should not change, and still use a COLLSCAN.
validateQueryPlan({
    query: {d: {$elemMatch: {$or: [{e: 6}, {g: 2}]}}},
    expectedStageCount: {"COLLSCAN": 1, "OR": 0},
    expectedDocIds: [3, 4]
});

// $or inside an $elemMatch that is indexed should use only IXSCAN.
assert.commandWorked(coll.createIndex({"d.e": 1}));
assert.commandWorked(coll.createIndex({"d.g": 1}));
validateQueryOR({
    query: {d: {$elemMatch: {$or: [{e: 10}, {g: 4}]}}},
    expectedStageCount: {"IXSCAN": 2, "COLLSCAN": 0},
    expectedDocIds: [5, 8],
});

// TODO SERVER-77601 remove this function, once supported in SBE.
// We prevented allowing MERGE_SORT plans with clustered collection scans, so the plan should
// fallback to using a collection scan.
function validateQuerySort() {
    let explain =
        coll.explain().find({$or: [{_id: {$lt: 1}}, {_id: {$gt: 8}}]}).sort({_id: 1}).finish();
    const winningPlan = getWinningPlan(explain.queryPlanner);
    let expectedStageCount = {"MERGE_SORT": 0, "COLLSCAN": 1, "CLUSTERED_IXSCAN": 0, "OR": 0};
    const shardMergeStage = haveShardMergeStage(winningPlan, "SHARD_MERGE_SORT");
    const shards = "shards" in winningPlan;
    for (var stage in expectedStageCount) {
        let planStages = getPlanStages(winningPlan, stage);
        assert(planStages, tojson(winningPlan));
        if (shardMergeStage || shards) {
            assert.gte(planStages.length,
                       expectedStageCount[stage],
                       "Expected " + stage + " to appear, but got plan: " + tojson(winningPlan));
        } else {
            assert.eq(planStages.length,
                      expectedStageCount[stage],
                      "Expected " + stage + " to appear, but got plan: " + tojson(winningPlan));
        }
    }
    assertCorrectResults({
        query: coll.find({$or: [{_id: {$lt: 1}}, {_id: {$gt: 8}}]}).sort({_id: 1}),
        expectedDocIds: [0, 9]
    });
}
validateQuerySort();

//
// These tests validate that $or queries with a text index work.
//

// Basic case $or with text and a clustered collection scan.
validateQueryOR({
    query: {$or: [{$text: {$search: "foo"}}, {_id: 1}]},
    expectedStageCount: {"CLUSTERED_IXSCAN": 1, "TEXT_MATCH": 1, "IXSCAN": 1},
    expectedDocIds: [0, 1, 5, 6]
});

// $or with a text index work with a clustered collection scan plan and a secondary index scan plan.
// We expected 2 IXSCAN nodes because the TEXT_MATCH stage has a IXSCAN node child, and there is an
// index scan plan for the {a: 9} predicate.
validateQueryOR({
    query: {$or: [{$text: {$search: "foo"}}, {_id: {$lt: 2}}, {a: 9}]},
    expectedStageCount: {"CLUSTERED_IXSCAN": 1, "TEXT_MATCH": 1, "IXSCAN": 2},
    expectedDocIds: [0, 1, 5, 6, 9]
});

// $or inside an and with a text index works.
validateQueryPlan({
    query: {$and: [{a: {$gte: 8}}, {$or: [{$text: {$search: "foo"}}, {c: {$gt: 10}}]}]},
    expectedStageCount: {"FETCH": 2, "IXSCAN": 2, "TEXT_MATCH": 1},
    expectedDocIds: [8, 9],
});

// $or inside an or with a text index works.
validateQueryOR({
    query: {$or: [{_id: {$gte: 8}}, {$or: [{$text: {$search: "foo"}}, {c: {$gt: 10}}]}]},
    expectedStageCount: {"FETCH": 2, "IXSCAN": 2, "TEXT_MATCH": 1, "CLUSTERED_IXSCAN": 1},
    expectedDocIds: [0, 5, 6, 7, 8, 9],
});

// $or with a text index and an unindexed field should still fail.
const err =
    assert.throws(() => coll.find({$or: [{$text: {$search: "foo"}}, {noIndex: 1}]}).toArray());
assert.commandFailedWithCode(err, ErrorCodes.NoQueryExecutionPlans);
