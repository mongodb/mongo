/**
 * Tests for that subplanning achieves good plan quality and avoids bias toward the first branch
 * and it enumerates more plans than whole query planning. In other words, test that rooted $or
 * queries do not suffer from known issues with contained $or (SERVER-36393 and SERVER-46904).
 * Note that or_use_clustered_collection.js also tests some of these scenarios, but specifically
 * for queries on clustered collections.
 *
 * @tags: [
 *   # Explain details will be different on sharded collections, and this test is focused on the
 *   # quality of the plan chosen, not the explain output.
 *   assumes_against_mongod_not_mongos,
 *   # The timeseries suite transforms the collection, breaking the test's index assertions.
 *   exclude_from_timeseries_crud_passthrough,
 *   # Changing server parameters is incompatible with stepdowns.
 *   does_not_support_stepdowns,
 *   # Explain for the aggregate command cannot run within a multi-document transaction
 *   does_not_support_transactions,
 *   # Explain command does not support read concerns other than local
 *   assumes_read_concern_local,
 * ]
 */
import {getPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {getPlanRankerMode} from "jstests/libs/query/cbr_utils.js";

// Ensure that subplanning is enabled.
if (
    !db.adminCommand({getParameter: 1, internalQueryPlanOrChildrenIndependently: true})
        .internalQueryPlanOrChildrenIndependently
) {
    jsTest.log.info("Skipping the test because subplanning is not enabled.");
    quit();
}

function indexesUsedByFindQuery(coll, query) {
    const explain = coll.explain().find(query).finish();
    const queryPlan = getWinningPlanFromExplain(explain);
    const stages = getPlanStages(queryPlan, "IXSCAN");
    return stages.map((stage) => stage.indexName);
}

// Test that plan selection avoids bias toward the first $or branch.
{
    const coll = db.subplanning_plan_quality;
    coll.drop();

    const docs = [];
    const N = 500;
    for (let i = 0; i < N; i++) {
        docs.push({a: i, b: N - i});
    }
    assert.commandWorked(coll.insertMany(docs));

    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));

    // The query below is designed to have two branches with one clearly superior index for each
    // branch: a and b, respectively. The first branch has enough results to fill a multi-planner
    // batch (101 documents), so whole-query multi-planning is not able to evaluate the second
    // branch and is unlikely to choose the ideal index "b" on the second branch.
    // Subplanning is needed to get the "b" index on the second branch.
    const q = {
        $or: [
            {a: {"$lte": 102}, b: {"$gte": 0}},
            {a: {"$gte": 0}, b: {"$lte": 102}},
        ],
    };

    const indexesUsed = indexesUsedByFindQuery(coll, q);
    assert(
        indexesUsed.includes("a_1"),
        "Expected index 'a_1' to be used in the winning plan, but it was not. Indexes used: " + tojson(indexesUsed),
    );
    assert(
        indexesUsed.includes("b_1"),
        "Expected index 'b_1' to be used in the winning plan, but it was not. Indexes used: " + tojson(indexesUsed),
    );

    // Skip the subplanning-disabled test under CBR, because CBR does not suffer from the branch
    // bias issue.
    if (getPlanRankerMode(db) === "multiPlanning") {
        // Temporarily disable subplanning and show that the winning plan misses the ideal index "b"
        // on the second branch.
        try {
            assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanOrChildrenIndependently: false}));
            assert(
                !indexesUsedByFindQuery(coll, q).includes("b_1"),
                "Without subplanning, we do not expect index 'b_1' to be used in the winning plan, due to planner limitations.",
            );
        } finally {
            assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanOrChildrenIndependently: true}));
        }
    }
}

// Test that plan selection evaluates a large space of plans for rooted $or queries.
{
    const coll = db.subplanning_plan_quality;
    coll.drop();

    // Insert documents that vary only in the value of the "j" field, so that only the "j" index
    // is useful for the query below, and the planner will have to enumerate past all the other
    // indexes before it gets to the "j" index.
    const docs = [];
    const N = 6;
    for (let i = 0; i < N; i++) {
        docs.push({a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: i});
    }
    assert.commandWorked(coll.insertMany(docs));

    // Define indexes on all 10 fields.
    for (const field of ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"]) {
        assert.commandWorked(coll.createIndex({[field]: 1}));
    }

    // Under whole query-plan planning, there are 10^6 = 1M possible plans to consider, so we will
    // not enumerate the ideal plan, which uses the "j" index on all branches.
    // Subplanning would only need to enumerate 10*6 = 60 plans across all the branches to explore
    // the full plan space, so it will find the ideal plan.
    const q = {
        $or: [
            {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: 0},
            {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: 1},
            {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: 2},
            {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: 3},
            {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: 4},
            {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: 5},
        ],
    };

    const indexesUsed = indexesUsedByFindQuery(coll, q);
    for (const i of ["a", "b", "c", "d", "e", "f", "g", "h", "i"]) {
        assert(
            !indexesUsed.includes(i + "_1"),
            "Index '" +
                i +
                "_1' was used but we expect only the index 'j_1' to be used. Indexes used: " +
                indexesUsed.join(", "),
        );
    }
    assert(
        indexesUsed.includes("j_1"),
        "Expected index 'j_1' to be used in the winning plan, but it was not. Indexes used: " + indexesUsed.join(", "),
    );

    // Temporarily disable subplanning and show that the winning plan misses the ideal index "j".
    try {
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanOrChildrenIndependently: false}));
        assert(
            !indexesUsedByFindQuery(coll, q).includes("j_1"),
            "Without subplanning, we do not expect index 'j_1' to be used in the winning plan, due to planner limitations.",
        );
    } finally {
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanOrChildrenIndependently: true}));
    }
}
