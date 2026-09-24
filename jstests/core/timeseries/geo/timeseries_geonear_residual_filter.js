/**
 * Tests a predicate above a $geoNear stage on a time-series collection.
 *
 * The residual filter pushdown into the $geoNear node does not apply here, because a $geoNear on a
 * time-series collection never plans a GEO_NEAR stage in the first place: it is rewritten into a
 * bucket-level predicate answered by a '2dsphere_bucket' index (or a collection scan for a flat
 * query, since 2d indexes are not supported on measurements), with the distance computation, the
 * event filter and the sort all happening above $_internalUnpackBucket. This test pins that plan
 * shape, so that extending the pushdown to time-series collections is a deliberate decision rather
 * than an accident, and checks that the predicate produces correct results either way.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # $geoNear must be the first stage in a pipeline.
 *   assumes_unsharded_collection,
 *   requires_fcv_91,
 *   uses_explain,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # Time series geo functionality requires pipeline optimization.
 *   requires_pipeline_optimization,
 * ]
 */

import {before, describe, it} from "jstests/libs/mochalite.js";
import {aggPlanHasStage} from "jstests/libs/query/analyze_plan.js";

describe("$geoNear residual filter pushdown on a time-series collection", function () {
    const coll = db[jsTestName()];

    before(function () {
        coll.drop();
        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}),
        );
        assert.commandWorked(coll.createIndex({loc: "2dsphere"}));
        // Distances from [0, 0]: _id 0 is at 0 meters, _id 1 at ~157000, _id 2 at ~314000.
        assert.commandWorked(
            coll.insert([
                {
                    _id: 0,
                    t: ISODate("2024-01-01T00:00:00Z"),
                    m: 1,
                    loc: {type: "Point", coordinates: [0, 0]},
                    k: 1,
                },
                {
                    _id: 1,
                    t: ISODate("2024-01-01T00:00:01Z"),
                    m: 1,
                    loc: {type: "Point", coordinates: [1, 1]},
                    k: 2,
                },
                {
                    _id: 2,
                    t: ISODate("2024-01-01T00:00:02Z"),
                    m: 2,
                    loc: {type: "Point", coordinates: [2, 2]},
                    k: 3,
                },
            ]),
        );
    });

    // $geoNear on a time-series collection requires 'key' and rejects 'query', so the predicate is
    // expressed as a $match right above the stage.
    function geoNearWithMatch(predicate) {
        return coll
            .aggregate([
                {
                    $geoNear: {
                        near: {type: "Point", coordinates: [0, 0]},
                        distanceField: "dist",
                        key: "loc",
                    },
                },
                {$match: predicate},
                {$project: {_id: 1}},
            ])
            .toArray()
            .map((doc) => doc._id);
    }

    it("applies a predicate on a measurement field", function () {
        assert.eq([1, 2], geoNearWithMatch({k: {$gte: 2}}), "unexpected results");
    });

    it("applies a predicate on the metaField", function () {
        assert.eq([2], geoNearWithMatch({m: 2}), "unexpected results");
    });

    it("applies an $expr predicate", function () {
        assert.eq([1], geoNearWithMatch({$expr: {$eq: ["$k", 2]}}), "unexpected results");
    });

    it("applies a predicate that matches nothing", function () {
        assert.eq([], geoNearWithMatch({k: 999}), "unexpected results");
    });

    it("does not plan a GEO_NEAR stage, so the residual filter pushdown cannot apply", function () {
        const explain = coll.explain().aggregate([
            {
                $geoNear: {
                    near: {type: "Point", coordinates: [0, 0]},
                    distanceField: "dist",
                    key: "loc",
                },
            },
            {$match: {k: {$gte: 2}}},
        ]);
        assert(
            !aggPlanHasStage(explain, "GEO_NEAR_2DSPHERE"),
            "unexpected GEO_NEAR_2DSPHERE stage",
            {
                explain,
            },
        );
        assert(!aggPlanHasStage(explain, "GEO_NEAR_2D"), "unexpected GEO_NEAR_2D stage", {explain});
    });
});
