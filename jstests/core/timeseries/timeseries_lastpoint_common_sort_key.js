/**
 * Tests that the lastpoint optimization is applied to special cases where top/bottom common sort
 * key optimization converts multiple tops/bottoms into one top or bottom.
 *
 * The details of the lastpoint optimization can be found at the companion test file
 * timeseries_lastpoint.js
 *
 * @tags: [
 *   requires_timeseries,
 *   # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # "Explain of a resolved view must be executed by mongos"
 *   directly_against_shardsvrs_incompatible,
 *   tenant_migration_incompatible,
 *   # The top/bottom common sort key optimization is available since FCV 8.0.
 *   requires_fcv_80,
 *   # Buckets being closed during moveCollection can cause the bucket ranges to differ.
 *   assumes_balancer_off,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getEngine, getPlanStage, getSingleNodeExplain} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

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
let lpa1 = undefined;  // lastpoint value of a for m = 1
let lpa2 = undefined;  // lastpoint value of a for m = 1

(function setupCollection() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));

    coll.insert({t: timestamps.t2, m: 1, x: 2, a: 12});  // create bucket #3
    coll.insert({t: timestamps.t4, m: 1, x: 4, a: 14});  // add to bucket #3
    coll.insert(
        {t: timestamps.t1, m: 1, x: 1, a: 11});  // earlier time => create bucket #2 (close #3)
    coll.insert(
        {t: timestamps.t5, m: 1, x: 5, a: 15});  // add to bucket #2, this is the lastpoint event
    lpx1 = 5;
    lpa1 = 15;
    coll.insert(
        {t: timestamps.t0, m: 1, x: 0, a: 10});  // earlier time => create bucket #1 (close #2)
    coll.insert({t: timestamps.t3, m: 1, x: 3, a: 13});  // add to bucket #1

    // An event with a different meta goes into a separate bucket.
    coll.insert({t: timestamps.t6, m: 2, x: 6, a: 16});
    lpx2 = 6;
    lpa2 = 16;

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

// Cases when the lastpoint optimization does apply. When there is no suitable index, the plan is:
// SORT + PROJECTION_SIMPLE + COLLSCAN + $group at the bucket level followed by unpack, etc.
// The default {m: 1, t: 1} index isn't suitable.
const casesLastpointOptimization = [
    // If all multiple top/bottom accumulators that use the same sort pattern can be merged into one
    // lastpoint top/bottom, the merged top/bottom can be optimized.
    {
        pipeline: [{
            $group: {
                _id: "$m",
                acc1: {$bottom: {sortBy: {t: 1}, output: ["$x"]}},
                acc2: {$bottom: {sortBy: {t: 1}, output: ["$a"]}},
            }
        }],
        expectedResult: [{_id: 1, acc1: [lpx1], acc2: [lpa1]}, {_id: 2, acc1: [lpx2], acc2: [lpa2]}]
    },
    {
        pipeline: [{
            $group: {
                _id: "$m",
                acc1: {$top: {sortBy: {t: -1}, output: ["$x"]}},
                acc2: {$top: {sortBy: {t: -1}, output: ["$a"]}},
            }
        }],
        expectedResult: [{_id: 1, acc1: [lpx1], acc2: [lpa1]}, {_id: 2, acc1: [lpx2], acc2: [lpa2]}]
    },
];

// When we expect the optimization to happen, we'll check the plan and the result of the query to
// confirm the correctness of rewrite.
(function testLastpointOptimization() {
    for (const {pipeline, expectedResult} of casesLastpointOptimization) {
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
            expected: expectedResult,
            actual: coll.aggregate(pipeline).toArray(),
            extraErrorMsg: `Expected result for pipeline ${tojson(pipeline)} with explain ${
                tojson(explainFull)}`
        });
    }
})();
