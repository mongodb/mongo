/**
 * Tests that a $limit (and $skip) following $geoNear is pushed down into the underlying near query
 * instead of being applied as a separate pipeline stage.
 *
 * @tags: [
 *   # $geoNear is not supported on views.
 *   incompatible_with_views,
 *   # The explain assertions expect the plan shape of an unsharded collection.
 *   assumes_unsharded_collection,
 *   # The timeseries cases below create their own timeseries collection.
 *   exclude_from_timeseries_crud_passthrough,
 *   requires_timeseries,
 *   # The pushdown is skipped when pipeline optimization is disabled.
 *   requires_pipeline_optimization,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    getAggPlanStage,
    getAggPlanStages,
    getExecutionStats,
    getSingleNodeExplain,
} from "jstests/libs/query/analyze_plan.js";
import {add2dsphereVersionIfNeeded} from "jstests/libs/query/geo_index_version_helpers.js";

const coll = db[jsTestName()];

const geoNear = {
    $geoNear: {near: {type: "Point", coordinates: [0, 0]}, distanceField: "dist", spherical: true},
};

const nonPushdownBarrier = {$match: {dist: {$gte: 0}}};

function ids(results) {
    return results.map((doc) => doc._id);
}

function assertPushedDownSkipLimit(explain, {skip = null, limit}) {
    const limitStages = getAggPlanStages(explain, "LIMIT");
    assert.eq(limitStages.length, 1, "expected exactly one LIMIT stage", {explain});
    assert.eq(limitStages[0].limitAmount, limit, "unexpected LIMIT amount", {explain});

    const skipStages = getAggPlanStages(explain, "SKIP");
    if (skip === null) {
        assert.eq(skipStages.length, 0, "unexpected SKIP stage", {explain});
    } else {
        assert.eq(skipStages.length, 1, "expected exactly one SKIP stage", {explain});
        assert.eq(skipStages[0].skipAmount, skip, "unexpected SKIP amount", {explain});
    }
}

describe("$geoNear $limit pushdown", function () {
    const numDocs = 100;

    before(function () {
        coll.drop();
        const docs = [];
        for (let i = 0; i < numDocs; i++) {
            docs.push({_id: i, loc: {type: "Point", coordinates: [i * 0.1, 0]}, group: i % 2});
        }
        assert.commandWorked(coll.insertMany(docs));
        assert.commandWorked(coll.createIndex({loc: "2dsphere"}));
    });

    after(function () {
        coll.drop();
    });

    function explainPipeline(pipeline, verbosity = "queryPlanner") {
        return coll.explain(verbosity).aggregate(pipeline);
    }

    it("removes $limit from the pipeline and applies it in the query plan", function () {
        const explain = explainPipeline([geoNear, {$limit: 3}]);
        assert.eq(getAggPlanStages(explain, "$limit").length, 0, "unexpected plan", {explain});
        assertPushedDownSkipLimit(explain, {limit: 3});
    });

    it("pushes down $skip together with $limit", function () {
        const explain = explainPipeline([geoNear, {$skip: 2}, {$limit: 3}]);
        assert.eq(getAggPlanStages(explain, "$skip").length, 0, "unexpected plan", {explain});
        assert.eq(getAggPlanStages(explain, "$limit").length, 0, "unexpected plan", {explain});
        assertPushedDownSkipLimit(explain, {skip: 2, limit: 3});
    });

    it("pushes down $limit past a stage that transforms documents one to one", function () {
        const explain = explainPipeline([geoNear, {$project: {dist: 1}}, {$limit: 3}]);
        assert.eq(getAggPlanStages(explain, "$limit").length, 0, "unexpected plan", {explain});
        assertPushedDownSkipLimit(explain, {limit: 3});
    });

    it("does not push down $limit past a stage that filters documents", function () {
        const explain = explainPipeline([geoNear, nonPushdownBarrier, {$limit: 3}]);
        assert.eq(getAggPlanStages(explain, "$limit").length, 1, "unexpected plan", {explain});
        assert.eq(null, getAggPlanStage(explain, "LIMIT"), "unexpected plan", {explain});
    });

    it("returns the same results as without the pushdown", function () {
        for (const [skip, limit] of [
            [0, 1],
            [0, 5],
            [3, 4],
            [10, 50],
            [95, 20],
            [0, 1000],
        ]) {
            const tail = [];
            if (skip > 0) {
                tail.push({$skip: skip});
            }
            tail.push({$limit: limit});
            const pushed = coll.aggregate([geoNear, ...tail]).toArray();
            const baseline = coll.aggregate([geoNear, nonPushdownBarrier, ...tail]).toArray();
            assert.eq(ids(pushed), ids(baseline), "unexpected result", {skip, limit});
            assert.eq(
                pushed.length,
                Math.max(0, Math.min(limit, numDocs - skip)),
                "unexpected result",
                {skip, limit},
            );
        }
    });

    it("applies the limit to documents that pass the 'query' filter", function () {
        const pipeline = [{$geoNear: {...geoNear.$geoNear, query: {group: 1}}}, {$limit: 4}];
        const results = coll.aggregate(pipeline).toArray();
        assert.eq(ids(results), [1, 3, 5, 7]);
    });

    it("applies the limit with 'minDistance' and 'maxDistance'", function () {
        const bounded = {
            $geoNear: {...geoNear.$geoNear, minDistance: 20000, maxDistance: 600000},
        };
        const pushed = coll.aggregate([bounded, {$limit: 3}]).toArray();
        const baseline = coll.aggregate([bounded, nonPushdownBarrier, {$limit: 3}]).toArray();
        assert.eq(pushed.length, 3);
        assert.eq(ids(pushed), ids(baseline));
    });

    it("examines fewer documents when the limit is small", function () {
        const explain = explainPipeline([geoNear, {$limit: 1}], "executionStats");
        assertPushedDownSkipLimit(explain, {limit: 1});
        // This test assumes unsharded collection, when running on mongod we get direct explain output
        // when test runs on mongos the explain output is wrapped in 'shards' so we need to unwrap.
        const [stats] = getExecutionStats(getSingleNodeExplain(explain));
        const baselineExplain = explainPipeline(
            [geoNear, nonPushdownBarrier, {$limit: 1}],
            "executionStats",
        );
        const [baselineStats] = getExecutionStats(getSingleNodeExplain(baselineExplain));
        assert.lt(stats.totalDocsExamined, baselineStats.totalDocsExamined, "unexpected plan", {
            explain,
            baselineExplain,
        });
    });

    it("does not move $sample ahead of $geoNear", function () {
        const results = coll.aggregate([geoNear, {$sample: {size: 3}}]).toArray();
        assert.eq(results.length, 3);
        for (const doc of results) {
            assert(doc.hasOwnProperty("dist"), "missing distance field", {doc});
        }
    });
});

describe("$geoNear $limit pushdown on a timeseries collection", function () {
    const tsColl = db[jsTestName() + "_ts"];
    const numDocs = 50;

    // $geoNear on the meta field runs as a near query over raw buckets, so the limit can be
    // applied by the executor. The key must be fully specified for $geoNear on timeseries.
    const metaGeoNear = {
        $geoNear: {
            near: {type: "Point", coordinates: [0, 0]},
            key: "tags.loc",
            distanceField: "tags.dist",
            spherical: true,
        },
    };
    // $geoNear on a measurement field cannot run until buckets are unpacked, so it is rewritten
    // into $match and $sort stages instead of a near query.
    const measurementGeoNear = {
        $geoNear: {
            near: {type: "Point", coordinates: [0, 0]},
            key: "loc",
            distanceField: "dist",
            spherical: true,
        },
    };

    before(function () {
        tsColl.drop();
        assert.commandWorked(
            db.createCollection(tsColl.getName(), {
                timeseries: {timeField: "time", metaField: "tags"},
            }),
        );
        const docs = [];
        for (let i = 0; i < numDocs; i++) {
            // Each measurement gets its own meta value so that each lands in its own bucket.
            docs.push({
                _id: i,
                time: ISODate("2026-01-01T00:00:00Z"),
                tags: {id: i, loc: {type: "Point", coordinates: [i * 0.1, 0]}},
                loc: {type: "Point", coordinates: [i * 0.1, 0]},
            });
        }
        assert.commandWorked(tsColl.insertMany(docs));
        assert.commandWorked(
            tsColl.createIndex({"tags.loc": "2dsphere"}, add2dsphereVersionIfNeeded()),
        );
        assert.commandWorked(tsColl.createIndex({loc: "2dsphere"}, add2dsphereVersionIfNeeded()));
    });

    after(function () {
        tsColl.drop();
    });

    it("pushes down the bucket-level $limit for $geoNear on the meta field", function () {
        const explain = tsColl.explain().aggregate([metaGeoNear, {$limit: 3}]);
        // $_internalUnpackBucket inserts a copy of the $limit ahead of itself to bound the number
        // of buckets read as one bucket can contain many measurements, that copy is pushed into the executor while the original remains after
        // unpacking to bound the number of measurements.
        assertPushedDownSkipLimit(explain, {limit: 3});
        const limits = getAggPlanStages(explain, "$limit");
        assert.eq(limits.length, 1, "unexpected plan", {explain});
        assert.eq(limits[0].$limit, 3, "unexpected plan", {explain});
    });

    it("returns the same results as without the pushdown for $geoNear on the meta field", function () {
        const all = ids(tsColl.aggregate([metaGeoNear]).toArray());
        assert.eq(all.length, numDocs);
        for (const [skip, limit] of [
            [0, 1],
            [0, 5],
            [3, 4],
            [45, 20],
        ]) {
            const tail = [];
            if (skip > 0) {
                tail.push({$skip: skip});
            }
            tail.push({$limit: limit});
            const pushed = tsColl.aggregate([metaGeoNear, ...tail]).toArray();
            assert.eq(ids(pushed), all.slice(skip, skip + limit), "unexpected result", {
                skip,
                limit,
            });
        }
    });

    it("does not push down $limit for $geoNear on a measurement field", function () {
        const explain = tsColl.explain().aggregate([measurementGeoNear, {$limit: 3}]);
        // $geoNear cannot run on buckets here, so it is rewritten into a distance computation and
        // a $sort after unpacking.
        assert.eq(getAggPlanStages(explain, "GEO_NEAR_2DSPHERE").length, 0, "unexpected plan", {
            explain,
        });
        assert.eq(getAggPlanStages(explain, "LIMIT").length, 0, "unexpected plan", {explain});
        assert.eq(getAggPlanStages(explain, "$limit").length, 0, "unexpected plan", {explain});

        // The $limit is absorbed into that $sort
        const sorts = getAggPlanStages(explain, "$sort");
        assert.eq(sorts.length, 1, "unexpected plan", {explain});
        assert.eq(sorts[0].$sort.limit, 3, "unexpected plan", {explain});
        const results = tsColl.aggregate([measurementGeoNear, {$limit: 3}]).toArray();
        assert.eq(ids(results), [0, 1, 2]);
    });
});
