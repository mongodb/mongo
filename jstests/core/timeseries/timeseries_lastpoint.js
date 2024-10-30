/**
 * Conceptually, the lastpoint optimization consists of adding bucket-level stages that would limit
 * the data to the bucket(s) that contain the lastpoint event(s). Then only these buckets are
 * unpacked and the original pipeline is run over the unpacked events.
 *
 * Computing a "lastpoint" involves defining an ordering and using a $group stage to pick the
 * boundary element. The ordering can come either from an explicit $sort stage or from the 'sortBy'
 * property of the top/bottom accumulators in the $group. We only do this optimization when ordering
 * on time. Note, that the actual sort pattern might include a prefix for the key that is used to
 * group by.
 *
 * The additional stages against the buckets collection rely on the control values for the time
 * field. The min/max control values for time have _different semantics_. Max is a true max accross
 * the events in the bucket but min is a _rounded down_ value per the collection settings. This
 * means that we can find the event with max t ({$top: {sortBy: {t: -1}, ...}
 * or {$bottom: {sortBy: {t: 1}, ...}}) accurately by sorting the buckets on
 * {control.time.max: -1} and taking the first bucket (then unpacking the bucket, etc.). We cannot
 * do the same when looking for the event with min t ({$top: {sortBy: {t: 1}, ...} or
 * {$bottom: {sortBy: {t: -1}, ...}}).
 *
 * The optimization would be valid for any group key that guarantees combining all events from a
 * given bucket into the same group (e.g. a const group key), however, we only support the case
 * when the group key is a single 'metaField' (whole or a sub-field, but not an expression).
 *
 * In presense of suitable indexes inserting the bucket level stages might enable futher
 * optimizations such as DISTINCT_SCAN.
 *
 * @tags: [
 *   requires_timeseries,
 *   # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # "Explain of a resolved view must be executed by mongos"
 *   directly_against_shardsvrs_incompatible,
 *   # Buckets being closed during resharding can cause the bucket ranges in this test to vary.
 *   assumes_balancer_off,
 *   # Some optimization is done in 7.2, some tests may fail prior to 7.2.
 *   requires_fcv_72,
 * ]
 *
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    getAggPlanStage,
    getEngine,
    getPlanStage,
    getSingleNodeExplain,
    isAggregationPlan
} from "jstests/libs/query/analyze_plan.js";

const coll = db.timeseries_lastpoint;
const bucketsColl = db.system.buckets.timeseries_lastpoint;

// The lastpoint optimization attempt to pick a bucket that would contain the event with max time
// and then only unpack that bucket. This function creates a collection with three buckets that
// include events that define the following time windows (for m: 1):
// #1: [t0*         t3]
// #2:     [t1*             t5] <- the optimization should pick this bucket, lastpoint event is t5
// #3:         [t2*     t4]
// NB: ti* are rounded down to the nearest minute
const timestamps = {
    // Ordered in time.
    t0: ISODate("2016-01-01T00:00:28Z"),
    t1: ISODate("2016-01-01T00:05:17Z"),
    t2: ISODate("2016-01-01T00:10:42Z"),
    t3: ISODate("2016-01-01T00:15:50Z"),
    t4: ISODate("2016-01-01T00:20:06Z"),
    t5: ISODate("2016-01-01T00:25:59Z"),
    t6: ISODate("2016-01-01T00:30:01Z"),
};
let lpx1 = undefined;  // lastpoint value of x for m = 1
let lpx2 = undefined;  // lastpoint value of x for m = 2
(function setupCollection() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));

    coll.insert({t: timestamps.t2, m: 1, x: 2});  // create bucket #3
    coll.insert({t: timestamps.t4, m: 1, x: 4});  // add to bucket #3
    coll.insert({t: timestamps.t1, m: 1, x: 1});  // earlier time => create bucket #2 (close #3)
    coll.insert({t: timestamps.t5, m: 1, x: 5});  // add to bucket #2, this is the lastpoint event
    lpx1 = 5;
    coll.insert({t: timestamps.t0, m: 1, x: 0});  // earlier time => create bucket #1 (close #2)
    coll.insert({t: timestamps.t3, m: 1, x: 3});  // add to bucket #1

    // An event with a different meta goes into a separate bucket.
    coll.insert({t: timestamps.t6, m: 2, x: 6});
    lpx2 = 6;

    // If this assert fails it would mean that bucket creation logic have changed. The lastpoint
    // optimization might still be correct but the tests below won't be checking some aspects of it
    // anymore.
    assertArrayEq({
        expected: [{t: timestamps.t3}, {t: timestamps.t4}, {t: timestamps.t5}, {t: timestamps.t6}],
        actual: bucketsColl.aggregate({$project: {t: "$control.max.t", _id: 0}}).toArray(),
        extraErrorMsg: `For the test data expect to create buckets with these max times but got ${
            tojson(bucketsColl.aggregate({$project: {control: 1, _id: 0}}).toArray())}`
    });
})();

// Cases when the lastpoint optimization doesn't apply.
const casesNoLastpointOptimization = [
    // First/last family of accumulators (quering for the event with min time).
    [{$sort: {t: 1}}, {$group: {_id: "$m", acc: {$first: "$x"}}}],
    [{$sort: {t: -1}}, {$group: {_id: "$m", acc: {$last: "$x"}}}],
    [{$sort: {m: 1, t: 1}}, {$group: {_id: "$m", acc: {$first: "$x"}}}],
    [{$sort: {m: 1, t: -1}}, {$group: {_id: "$m", acc: {$last: "$x"}}}],
    [{$sort: {m: -1, t: 1}}, {$group: {_id: "$m", acc: {$first: "$x"}}}],
    [{$sort: {m: -1, t: -1}}, {$group: {_id: "$m", acc: {$last: "$x"}}}],

    // Top/bottom family of accumulators (quering for the event with min time).
    [{$group: {_id: "$m", acc: {$top: {sortBy: {t: 1}, output: ["$x"]}}}}],
    [{$group: {_id: "$m", acc: {$bottom: {sortBy: {t: -1}, output: ["$x"]}}}}],
    [{$group: {_id: "$m", acc: {$top: {sortBy: {m: 1, t: 1}, output: ["$x"]}}}}],
    [{$group: {_id: "$m", acc: {$bottom: {sortBy: {m: 1, t: -1}, output: ["$x"]}}}}],
    [{$group: {_id: "$m", acc: {$top: {sortBy: {m: -1, t: 1}, output: ["$x"]}}}}],
    [{$group: {_id: "$m", acc: {$bottom: {sortBy: {m: -1, t: -1}, output: ["$x"]}}}}],

    // TopN/bottomN family of accumulators (quering for the event with min time).
    [{$group: {_id: "$m", acc: {$topN: {n: 1, sortBy: {t: 1}, output: ["$x"]}}}}],
    [{$group: {_id: "$m", acc: {$bottomN: {n: 1, sortBy: {t: -1}, output: ["$x"]}}}}],
    [{$group: {_id: "$m", acc: {$topN: {n: 1, sortBy: {m: 1, t: 1}, output: ["$x"]}}}}],
    [{$group: {_id: "$m", acc: {$bottomN: {n: 1, sortBy: {m: 1, t: -1}, output: ["$x"]}}}}],
    [{$group: {_id: "$m", acc: {$topN: {n: 1, sortBy: {m: -1, t: 1}, output: ["$x"]}}}}],
    [{$group: {_id: "$m", acc: {$bottomN: {n: 1, sortBy: {m: -1, t: -1}, output: ["$x"]}}}}],

    // TopN/bottomN family of accumulators with n > 1 or non-const (even if it always evals to 1).
    [{$group: {_id: "$m", acc: {$bottomN: {n: 2, sortBy: {t: 1}, output: ["$x"]}}}}],
    [{$group: {_id: "$m", acc: {$topN: {n: 2, sortBy: {t: -1}, output: ["$x"]}}}}],
    [{
        $group: {
            _id: "$m",
            acc: {
                $bottomN: {
                    n: {$cond: {if: {$lte: ["$m", 1024]}, then: 1, else: 2}},
                    sortBy: {t: 1},
                    output: ["$x"]
                }
            }
        }
    }],
    [{
        $group: {
            _id: "$m",
            acc: {
                $topN: {
                    n: {$cond: {if: {$lte: ["$m", 1024]}, then: 1, else: 2}},
                    sortBy: {t: -1},
                    output: ["$x"]
                }
            }
        }
    }],

    // If there are multiple top/bottom accumulators, even if all of them are eligible for the
    // optimization, we don't do it.
    [{
        $group: {
            _id: "$m",
            acc1: {$bottom: {sortBy: {t: 1}, output: ["$x"]}},
            acc2: {$top: {sortBy: {t: -1}, output: ["$y"]}},
        }
    }],

    // If there are accumulators, other than the supported ones or if mixing direction.
    [{$sort: {t: 1}}, {$group: {_id: "$m", acc1: {$last: "$x"}, acc2: {$max: " $y"}}}],
    [{$sort: {t: 1}}, {$group: {_id: "$m", acc1: {$last: "$x"}, acc2: {$first: " $y"}}}],

    // If there is an event filter, the optimization doesn't apply because the filter might skip
    // _all_ events in the bucket chosen by the bucket-level stages.
    [{$match: {y: 42}}, {$sort: {t: 1}}, {$group: {_id: "$m", acc: {$last: "$x"}}}],
    [{$match: {y: 42}}, {$group: {_id: "$m", acc: {$bottom: {sortBy: {t: 1}, output: ["$x"]}}}}],

    // If there is a limit after $sort, the optimization doesn't apply (as the limit might cut
    // off the max time depending on the direction of the sort).
    [{$sort: {t: 1}}, {$limit: 5}, {$group: {_id: "$m", acc: {$last: "$x"}}}],
    [{$sort: {t: -1}}, {$limit: 5}, {$group: {_id: "$m", acc: {$first: "$x"}}}],

    // Not grouping by a single 'metaField' (some of these groups, in theory, could be supported by
    // the lastpoint optimization, but it hasn't been implemented yet).
    [{$group: {_id: null, acc: {$bottom: {sortBy: {t: 1}, output: ["$x"]}}}}],
    [{$group: {_id: "$y", acc: {$bottom: {sortBy: {t: 1}, output: ["$x"]}}}}],
    [{
        $group: {
            _id: {$dateTrunc: {date: "$t", unit: "hour", binSize: 1}},
            acc: {$bottom: {sortBy: {t: 1}, output: ["$x"]}}
        }
    }],
    [{$group: {_id: {a: "$m.a", b: "$m.b"}, acc: {$bottom: {sortBy: {t: 1}, output: ["$x"]}}}}],
    [{$group: {_id: {$add: [1, "$m"]}, acc: {$bottom: {sortBy: {t: 1}, output: ["$x"]}}}}],
    [
        {$addFields: {mm: {$add: [1, "$m"]}}},
        {$group: {_id: "$mm", acc: {$bottom: {sortBy: {t: 1}, output: ["$x"]}}}}
    ],

    // Fields computed from 'metaField' cannot be used in the $group stage. Pushing down the
    // computed fields 'mm' would disable the last point optimization, as the optimization relies
    // that the 'control' block summaries which may have been invlidated by the $addFields pushdown.
    [
        {$addFields: {mm: {$add: [42, "$m"]}}},
        {$group: {_id: "$m", acc: {$bottom: {sortBy: {t: 1}, output: ["$x", "$mm"]}}}}
    ],
];

// Cases when the lastpoint optimization does apply. When there is no suitable index, the plan is:
// SORT + PROJECTION_SIMPLE + COLLSCAN + $group at the bucket level followed by unpack, etc.
// The default {m: 1, t: 1} index isn't suitable.
const casesLastpointOptimization = [
    // First/last family of accumulators.
    {
        pipeline: [{$sort: {t: 1}}, {$group: {_id: "$m", acc: {$last: "$x"}}}],
        expectedResult: [{_id: 1, acc: lpx1}, {_id: 2, acc: lpx2}]
    },
    {
        pipeline: [{$sort: {t: -1}}, {$group: {_id: "$m", acc: {$first: "$x"}}}],
        expectedResult: [{_id: 1, acc: lpx1}, {_id: 2, acc: lpx2}]
    },
    {
        pipeline: [{$sort: {m: 1, t: 1}}, {$group: {_id: "$m", acc: {$last: "$x"}}}],
        expectedResult: [{_id: 1, acc: lpx1}, {_id: 2, acc: lpx2}]
    },
    {
        pipeline: [{$sort: {m: 1, t: -1}}, {$group: {_id: "$m", acc: {$first: "$x"}}}],
        expectedResult: [{_id: 1, acc: lpx1}, {_id: 2, acc: lpx2}]
    },
    {
        pipeline: [{$sort: {m: -1, t: 1}}, {$group: {_id: "$m", acc: {$last: "$x"}}}],
        expectedResult: [{_id: 1, acc: lpx1}, {_id: 2, acc: lpx2}]
    },
    {
        pipeline: [{$sort: {m: -1, t: -1}}, {$group: {_id: "$m", acc: {$first: "$x"}}}],
        expectedResult: [{_id: 1, acc: lpx1}, {_id: 2, acc: lpx2}]
    },

    // Top/bottom family of accumulators.
    {
        pipeline: [{$group: {_id: "$m", acc: {$bottom: {sortBy: {t: 1}, output: ["$x"]}}}}],
        expectedResult: [{_id: 1, acc: [lpx1]}, {_id: 2, acc: [lpx2]}]
    },
    {
        pipeline: [{$group: {_id: "$m", acc: {$top: {sortBy: {t: -1}, output: ["$x"]}}}}],
        expectedResult: [{_id: 1, acc: [lpx1]}, {_id: 2, acc: [lpx2]}]
    },
    {
        pipeline: [{$group: {_id: "$m", acc: {$bottom: {sortBy: {m: 1, t: 1}, output: ["$x"]}}}}],
        expectedResult: [{_id: 1, acc: [lpx1]}, {_id: 2, acc: [lpx2]}]
    },
    {
        pipeline: [{$group: {_id: "$m", acc: {$top: {sortBy: {m: 1, t: -1}, output: ["$x"]}}}}],
        expectedResult: [{_id: 1, acc: [lpx1]}, {_id: 2, acc: [lpx2]}]
    },
    {
        pipeline: [{$group: {_id: "$m", acc: {$bottom: {sortBy: {m: -1, t: 1}, output: ["$x"]}}}}],
        expectedResult: [{_id: 1, acc: [lpx1]}, {_id: 2, acc: [lpx2]}]
    },
    {
        pipeline: [{$group: {_id: "$m", acc: {$top: {sortBy: {m: -1, t: -1}, output: ["$x"]}}}}],
        expectedResult: [{_id: 1, acc: [lpx1]}, {_id: 2, acc: [lpx2]}]
    },

    // TopN/bottomN family of accumulators.
    {
        pipeline: [{$group: {_id: "$m", acc: {$bottomN: {n: 1, sortBy: {t: 1}, output: ["$x"]}}}}],
        expectedResult: [{_id: 1, acc: [[lpx1]]}, {_id: 2, acc: [[lpx2]]}]
    },
    {
        pipeline: [{$group: {_id: "$m", acc: {$topN: {n: 1, sortBy: {t: -1}, output: ["$x"]}}}}],
        expectedResult: [{_id: 1, acc: [[lpx1]]}, {_id: 2, acc: [[lpx2]]}]
    },
    {
        pipeline:
            [{$group: {_id: "$m", acc: {$bottomN: {n: 1, sortBy: {m: 1, t: 1}, output: ["$x"]}}}}],
        expectedResult: [{_id: 1, acc: [[lpx1]]}, {_id: 2, acc: [[lpx2]]}]
    },
    {
        pipeline:
            [{$group: {_id: "$m", acc: {$topN: {n: 1, sortBy: {m: 1, t: -1}, output: ["$x"]}}}}],
        expectedResult: [{_id: 1, acc: [[lpx1]]}, {_id: 2, acc: [[lpx2]]}]
    },
    {
        pipeline:
            [{$group: {_id: "$m", acc: {$bottomN: {n: 1, sortBy: {m: -1, t: 1}, output: ["$x"]}}}}],
        expectedResult: [{_id: 1, acc: [[lpx1]]}, {_id: 2, acc: [[lpx2]]}]
    },
    {
        pipeline:
            [{$group: {_id: "$m", acc: {$topN: {n: 1, sortBy: {m: -1, t: -1}, output: ["$x"]}}}}],
        expectedResult: [{_id: 1, acc: [[lpx1]]}, {_id: 2, acc: [[lpx2]]}]
    },

    // Multiple $first ($last) are supported. As well as multiple outputs in top/bottom.
    {
        pipeline:
            [{$sort: {t: 1}}, {$group: {_id: "$m", acc1: {$last: "$x"}, acc2: {$last: "$y"}}}],
        expectedResult: [{_id: 1, acc1: lpx1, acc2: null}, {_id: 2, acc1: lpx2, acc2: null}]
    },
    {
        pipeline:
            [{$sort: {t: -1}}, {$group: {_id: "$m", acc1: {$first: "$x"}, acc2: {$first: "$y"}}}],
        expectedResult: [{_id: 1, acc1: lpx1, acc2: null}, {_id: 2, acc1: lpx2, acc2: null}]
    },
    {
        pipeline: [{$group: {_id: "$m", acc: {$bottom: {sortBy: {t: 1}, output: ["$x", "$y"]}}}}],
        expectedResult: [{_id: 1, acc: [lpx1, null]}, {_id: 2, acc: [lpx2, null]}]
    },
    {
        pipeline:
            [{$group: {_id: "$m", acc: {$topN: {n: 1, sortBy: {t: -1}, output: ["$x", "$y"]}}}}],
        expectedResult: [{_id: 1, acc: [[lpx1, null]]}, {_id: 2, acc: [[lpx2, null]]}]
    },

    // A filter on 'metaField' should not prevent this optimization.
    {
        pipeline:
            [{$match: {m: 1}}, {$sort: {m: -1, t: 1}}, {$group: {_id: "$m", acc: {$last: "$x"}}}],
        expectedResult: [{_id: 1, acc: lpx1}]
    },
    {
        pipeline: [
            {$match: {m: {$lte: 1}}},
            {$group: {_id: "$m", acc: {$bottom: {sortBy: {t: 1}, output: ["$x"]}}}}
        ],
        expectedResult: [{_id: 1, acc: [lpx1]}]
    },
    {
        pipeline: [
            {$match: {m: {$ne: 2}}},
            {$group: {_id: "$m", acc: {$bottomN: {n: 1, sortBy: {t: 1}, output: ["$x"]}}}}
        ],
        expectedResult: [{_id: 1, acc: [[lpx1]]}]
    }
];

// When there is a suitable index, DISTINCT_SCAN optimization should kick in. We only sanity test
// with a couple of pipelines because the correctness of DISTINCT_SCAN is verified elsewhere, so
// here we check that the rewrite we do for "lastpoint" does enable it.
const casesLastpointWithDistinctScan = [
    {
        pipeline: [{$sort: {t: 1}}, {$group: {_id: "$m", acc: {$last: "$x"}}}],
        suitableIndex: {m: -1, t: -1},
        expectedResult: [{_id: 1, acc: lpx1}, {_id: 2, acc: lpx2}]
    },
    {
        pipeline: [{$group: {_id: "$m", acc: {$bottom: {sortBy: {t: 1}, output: ["$x"]}}}}],
        suitableIndex: {m: -1, t: -1},
        expectedResult: [{_id: 1, acc: [lpx1]}, {_id: 2, acc: [lpx2]}]
    },
    {
        pipeline: [{$group: {_id: "$m", acc: {$topN: {n: 1, sortBy: {t: -1}, output: ["$x"]}}}}],
        suitableIndex: {m: 1, t: -1},
        expectedResult: [{_id: 1, acc: [[lpx1]]}, {_id: 2, acc: [[lpx2]]}]
    },
    {
        pipeline: [{$match: {m: 1}}, {$sort: {t: -1}}, {$group: {_id: "$m", acc: {$first: "$x"}}}],
        suitableIndex: {m: 1, t: -1},
        expectedResult: [{_id: 1, acc: lpx1}]
    },
];

// When we don't expect the optimization to happen, we'll just check the plan. For this the data in
// the collection is irrelevant as long as the collection isn't empty.
(function testNoOptimization() {
    for (const pipeline of casesNoLastpointOptimization) {
        const explainFull = assert.commandWorked(coll.explain().aggregate(pipeline));
        const explain = getSingleNodeExplain(explainFull);

        // When the lastpoint optimization doesn't apply there must be no group stage at the buckets
        // collection level (that is, before unpacking).
        if (getEngine(explain) === "classic") {
            for (const stage of explain.stages) {
                if (stage.hasOwnProperty("$_internalUnpackBucket")) {
                    break;
                }
                assert(
                    !stage.hasOwnProperty("$group"),
                    `Without lastpoint opt there should be no group before unpack but for pipeline ${
                        tojson(pipeline)} got ${tojson(explain)}`);
            }
        } else {
            // The pipeline was executed in SBE. The input to 'UNPACK_TS_BUCKET' cannot be a group.
            const unpack = isAggregationPlan(explain) ? getAggPlanStage(explain, "UNPACK_TS_BUCKET")
                                                      : getPlanStage(explain, "UNPACK_TS_BUCKET");
            assert.neq(
                "GROUP",
                unpack.inputStage.stage,
                `Without lastpoint opt there should be no group before unpack but for pipeline ${
                    tojson(pipeline)} got ${tojson(explain)}`);
        }
    }
})();

// When we expect the optimization to happen, we'll check the plan and the result of the query to
// confirm the correctness of rewrite.
(function testLastpointOptimization() {
    for (const testCase of casesLastpointOptimization) {
        const pipeline = testCase.pipeline;
        const explainFull = assert.commandWorked(coll.explain().aggregate(pipeline));
        const explain = getSingleNodeExplain(explainFull);

        // There must be a group at the buckets collection level (that is, before unpack).
        if (getEngine(explain) === "classic") {
            for (const stage of explain.stages) {
                if (stage.hasOwnProperty("$group")) {
                    break;
                }
                assert(!stage.hasOwnProperty("$_internalUnpackBucket"),
                       `Lastpoint opt should have inserted group before unpack for pipeline ${
                           tojson(pipeline)} but got ${tojson(explainFull)}`);
            }
        } else {
            // The lastpoint opt currently isn't lowered to SBE.
            assert(false,
                   `Lastpoint opt isn't implemented in SBE for pipeline ${
                       tojson(pipeline)} but got ${tojson(explainFull)}`);
        }

        // Check that the result matches the expected by the test case.
        assertArrayEq({
            expected: testCase.expectedResult,
            actual: coll.aggregate(pipeline).toArray(),
            extraErrorMsg: `Expected result for pipeline ${tojson(pipeline)} with explain ${
                tojson(explainFull)}`
        });
    }
})();

// When we expect the optimization to happen and there is a suitable index, it opens the door to
// the optimization that replaces the buckets level group with distinct scan over the index. We
// won't validate the distinct scan itself as it should have its own tests outside of lastpoint opt.
(function testLastpointOptimizationWithDistinctScan() {
    for (const testCase of casesLastpointWithDistinctScan) {
        coll.dropIndexes();
        coll.createIndex(testCase.suitableIndex);

        const pipeline = testCase.pipeline;
        const explainFull = assert.commandWorked(coll.explain().aggregate(pipeline));
        const explain = getSingleNodeExplain(explainFull);

        // There must be a group at the buckets collection level (that is, before unpack).
        if (getEngine(explain) === "classic") {
            for (const stage of explain.stages) {
                if (stage.hasOwnProperty("$groupByDistinctScan")) {
                    break;
                }
                assert(!stage.hasOwnProperty("$_internalUnpackBucket"),
                       `Lastpoint opt should have enabled distinct scan for pipeline ${
                           tojson(pipeline)} with index ${tojson(testCase.suitableIndex)} but got ${
                           tojson(explainFull)}`);
            }
        } else {
            // The distinct scan opt currently isn't lowered to SBE.
            assert(false,
                   `Lastpoint opt isn't implemented in SBE for pipeline ${
                       tojson(pipeline)} but got ${tojson(explainFull)}`);
        }

        // Check that the result matches the expected by the test case.
        assertArrayEq({
            expected: testCase.expectedResult,
            actual: coll.aggregate(pipeline).toArray(),
            extraErrorMsg: `Expected result for pipeline ${tojson(pipeline)} with explain ${
                tojson(explainFull)}`
        });
    }
})();
