/**
 * Tests that a $expr match expression can be split into two parts when partially dependent on a
 * computed field. This permits the independent portion of the $expr to be pushed down and
 * potentially to use an index.
 *
 * In particular, this test is designed to demonstrate a fix for SERVER-106505. In this ticket, the
 * $expr eligible for splitting is in a correlated $lookup subpipeline. The failure to split
 * resulted in the $sequentialCache stage interfering with $match pushdown, ultimately preventing
 * the inner pipeline from using an available index. This test proves that this scenario is fixed
 * and the inner side can now take advantage of the index.
 *
 * @tags: [
 *   requires_pipeline_optimization,
 *   do_not_wrap_aggregations_in_facets,
 *   # This test verifies behavior added in SERVER-106505 which is not present in older versions.
 *   requires_fcv_83,
 *   # If the shard list is not stable in sharding passthroughs, then the $lookup may end up running
 *   # as part of the merging pipeline. But this test assumes that the $lookup is pushed down to the
 *   # shards.
 *   assumes_stable_shard_list,
 * ]
 */

import {getAggPlanStages, getPlanStages, getQueryPlanners} from "jstests/libs/query/analyze_plan.js";

let outerColl = db.outer_coll;
let innerColl = db.inner_coll;
outerColl.drop();
innerColl.drop();

assert.commandWorked(outerColl.insert({_id: 1, x: 1, y: 3, z: 11, fk: 190}));
assert.commandWorked(outerColl.insert({_id: 2, x: 2, y: 4, z: 12, fk: 191}));

for (let i = 0; i < 1000; i++) {
    assert.commandWorked(innerColl.insert({_id: i, inner_id: i}));
}

assert.commandWorked(outerColl.createIndex({x: 1}));
assert.commandWorked(innerColl.createIndex({inner_id: 1}));

// The simplest case is an $projection which adds a computed field upon which an $expr is partially
// dependent. Show that the independent part of the $expr can be pushed down and can use an index.
(function testBasicSplittingToPushdownPastProjection() {
    let pipeline = [
        {$project: {_id: 0, x: 1, z: 1, computed: {$add: ["$y", 1]}}},
        {
            $match: {
                $expr: {
                    $and: [{$eq: ["$x", 2]}, {$eq: ["$computed", 5]}, {$eq: [{$mod: ["$z", 2]}, 0]}],
                },
            },
        },
    ];

    let result = outerColl.aggregate(pipeline).toArray();
    assert.eq(result, [{x: 2, z: 12, computed: 5}]);

    let explain = outerColl.explain("queryPlanner").aggregate(pipeline);
    let queryPlannerSections = getQueryPlanners(explain);

    for (let section of queryPlannerSections) {
        // Make sure that we push down an $expr containing both the $eq on "$x" and the $mod
        // involving "$z". Neither of these predicates depend on the computed field, so they can be
        // split apart from the predicate on "$computed" and pushed down.
        //
        // In older versions, we lacked the ability to split $expr for pushdown. In those versions
        // we would generate and push down a $_internalExprEq on field "x" but we would be unable to
        // push down the $mod expression. This test proves that we can now push down arbitrary
        // expressions inside the $expr so long as they do not depend on the computed fields.
        assert.eq(
            section.parsedQuery,
            {
                $and: [
                    {
                        $expr: {
                            $and: [{$eq: ["$x", {$const: 2}]}, {$eq: [{$mod: ["$z", {$const: 2}]}, {$const: 0}]}],
                        },
                    },
                    {x: {$_internalExprEq: 2}},
                ],
            },
            explain,
        );
    }

    // Verify that the $eq predicate on "x" results in an index scan with the expected bounds.
    let ixscanStages = getPlanStages(explain, "IXSCAN");
    assert.gt(ixscanStages.length, 0, explain);
    for (let ixscanStage of ixscanStages) {
        assert.neq(ixscanStage, null, explain);
        assert.eq(ixscanStage.keyPattern, {x: 1}, explain);
        assert.eq(ixscanStage.indexBounds, {x: ["[2.0, 2.0]"]}, explain);
    }
})();

// This pipeline is designed to reproduce SERVER-106505. Because of how optimization of the inner
// pipeline interacts with the $sequentialCache stage, the ability to split $expr into dependent and
// independent parts (when the $expr is only partially independent) is necessary to allow index use
// on the inner side.
(function testSplittingInCorrelatedLookup() {
    let pipeline = [
        {
            $lookup: {
                from: innerColl.getName(),
                let: {
                    correlated_var: "$fk",
                },
                pipeline: [
                    {
                        $project: {
                            inner_id: 1,
                            computed_field: {$literal: "foo"},
                        },
                    },
                    {
                        $match: {
                            // This expression needs to get split in order to use an index on the
                            // inner side since one of the conjuncts depends on a computed field and
                            // the other does not. Prior to the fix for SERVER-106505, the
                            // combination of our inability to split $expr for pushdown and
                            // interference from $sequentialCache prevented index use.
                            $expr: {
                                $and: [
                                    {
                                        $eq: ["$inner_id", "$$correlated_var"],
                                    },
                                    {
                                        $eq: ["$computed_field", "foo"],
                                    },
                                ],
                            },
                        },
                    },
                ],
                as: "join_result",
            },
        },
        {$unwind: "$join_result"},
        {$project: {_id: 0, fk: 1, join_result: 1}},
        {$sort: {fk: 1}},
    ];

    let result = outerColl.aggregate(pipeline).toArray();
    assert.eq(result, [
        {fk: 190, join_result: {_id: 190, inner_id: 190, computed_field: "foo"}},
        {fk: 191, join_result: {_id: 191, inner_id: 191, computed_field: "foo"}},
    ]);

    // Gather the $lookup execution stats and prove that the inner side probed a single index key
    // for each of the two documents on the outer side. This proves that the $expr was successfully
    // split, allowing the predicate on "inner_id" to be pushed down and avoiding a collection scan.
    let explain = outerColl.explain("executionStats").aggregate(pipeline);
    let lookupStages = getAggPlanStages(explain, "$lookup");
    assert.neq(lookupStages, null, explain);
    assert.gt(lookupStages.length, 0, explain);

    // The runtime stats are not accurately reported in sharding passthroughs due to SERVER-112893.
    // Therefore, we only make assertions about the runtime stats in the unsharded case.
    if (lookupStages.length === 1) {
        let lookupStage = lookupStages[0];
        assert.eq(lookupStage.totalDocsExamined, 2, explain);
        assert.eq(lookupStage.totalKeysExamined, 2, explain);
        assert.eq(lookupStage.collectionScans, 0, explain);
        assert.eq(lookupStage.indexesUsed.length, 1, explain);
        assert.eq(lookupStage.indexesUsed[0], "inner_id_1", explain);
    }
})();
