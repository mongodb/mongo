/**
 * Tests that an unfiltered count ($group with $sum or $count aggregation stage) plans a COUNT_SCAN over
 * the _id index instead of a COLLSCAN.
 *
 * @tags: [
 *   requires_fcv_91,
 *   uses_explain,
 *   assumes_unsharded_collection,
 *   requires_fastcount,
 *   # Timeseries collections do not have a regular _id index to count scan.
 *   exclude_from_timeseries_crud_passthrough,
 *   # setParameter may return different values after a failover.
 *   does_not_support_stepdowns,
 *   # COUNT_SCAN is not used for views.
 *   incompatible_with_views,
 *   # COUNT_SCAN optimization not performed on clustered collections
 *   expects_explicit_underscore_id_index,
 *   # Test changes server parameter nodes, which requires a stable shard list.
 *   assumes_stable_shard_list,
 *   # Explains of the aggregate command cannot run within a multi-document transaction.
 *   does_not_support_transactions,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

const coll = db.count_scan_unfiltered;
const kDocCount = 1000;
const testCases = [[{$count: "n"}], [{$group: {_id: 1, n: {$sum: 1}}}]];

function getCountScanStage(pipeline) {
    const explain = coll.explain().aggregate(pipeline);
    return getPlanStage(getWinningPlanFromExplain(explain, false), "COUNT_SCAN");
}

describe("unfiltered count planning", function () {
    before(function () {
        coll.drop();
        assert.commandWorked(
            coll.insertMany(Array.from({length: kDocCount}, (_, i) => ({_id: i, a: i * 5}))),
        );
        // A secondary index that is not applicable to an unfiltered count; the planner should
        // still fall back to a COUNT_SCAN of the _id index.
        assert.commandWorked(coll.createIndex({a: 1}));
    });

    after(function () {
        coll.drop();
    });

    it("plans a COUNT_SCAN over the _id index for an unfiltered count", function () {
        for (const pipeline of testCases) {
            const countScanStage = getCountScanStage(pipeline);
            assert(countScanStage, "expected a COUNT_SCAN stage");
            assert.eq(
                {_id: 1},
                countScanStage.keyPattern,
                "expected the COUNT_SCAN to be over the _id index",
            );
            assert.eq(kDocCount, coll.aggregate(pipeline).toArray()[0].n);
        }
    });

    it("uses a COLLSCAN for an unfiltered count hinted with $natural", function () {
        // A $natural hint forces a collection scan regardless of direction, so it should override
        // the COUNT_SCAN optimization.
        for (const pipeline of testCases) {
            for (const direction of [1, -1]) {
                const hint = {$natural: direction};
                const explain = coll.explain().aggregate(pipeline, {hint});
                const winningPlan = getWinningPlanFromExplain(explain, false);
                assert.eq(
                    null,
                    getPlanStage(winningPlan, "COUNT_SCAN"),
                    "did not expect a COUNT_SCAN stage",
                    {explain},
                );
                assert(getPlanStage(winningPlan, "COLLSCAN"), "expected a COLLSCAN stage", {
                    explain,
                });
                assert.eq(kDocCount, coll.aggregate(pipeline, {hint}).toArray()[0].n);
            }
        }
    });

    it("falls back to a COLLSCAN for an unfiltered count when internalQueryPlannerEnableCountScanForUnfilteredCount is disabled", function () {
        runWithParamsAllNonConfigNodes(
            db,
            {"internalQueryPlannerEnableCountScanForUnfilteredCount": false},
            () => {
                for (const pipeline of testCases) {
                    assert.eq(
                        null,
                        getCountScanStage(pipeline),
                        "did not expect a COUNT_SCAN stage",
                    );
                    assert.eq(kDocCount, coll.aggregate(pipeline).toArray()[0].n);
                }
            },
        );
    });
});
