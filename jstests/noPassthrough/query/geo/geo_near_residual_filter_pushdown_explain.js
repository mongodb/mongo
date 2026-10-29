/**
 * Tests the plan shape and the execution stats of a $geoNear query with a residual predicate. The
 * planner pushes such a predicate into the $geoNear node instead of leaving a FETCH stage above it,
 * so:
 *  - the winning plan has no FETCH above the GEO_NEAR stage,
 *  - the GEO_NEAR stage reports the predicate as its 'filter',
 *  - documents rejected by the predicate are still examined, but not returned.
 *
 * This runs outside of the passthrough suites because it asserts on the shape of the winning plan.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

let conn;
let coll;

before(function () {
    conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod failed to start");
    coll = conn.getDB("test").geo_near_residual_filter_pushdown_explain;
});

after(function () {
    MongoRunner.stopMongod(conn);
});

describe("$geoNear residual filter pushdown plan shape", function () {
    before(function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({loc: "2dsphere"}));
        assert.commandWorked(coll.createIndex({k: 1}));
        const docs = [];
        for (let i = 0; i < 20; ++i) {
            docs.push({_id: i, loc: {type: "Point", coordinates: [i / 10, i / 10]}, k: i % 2});
        }
        assert.commandWorked(coll.insert(docs));
    });

    function explainGeoNear(query, verbosity) {
        return coll.explain(verbosity).aggregate([
            {
                $geoNear: {
                    near: {type: "Point", coordinates: [0, 0]},
                    distanceField: "dist",
                    query: query,
                },
            },
        ]);
    }

    it("does not leave a FETCH stage above the GEO_NEAR stage", function () {
        const explain = explainGeoNear({k: 1});
        // The near stage owns a FETCH of its own below itself, so this asserts on the root of the
        // winning plan rather than on the absence of FETCH anywhere in the plan.
        const winningPlan = getWinningPlanFromExplain(explain);
        assert.eq("GEO_NEAR_2DSPHERE", winningPlan.stage, "unexpected root stage", {explain});
    });

    it("reports the pushed-down predicate as the filter of the GEO_NEAR stage", function () {
        const explain = explainGeoNear({k: 1});
        const geoNear = getPlanStage(getWinningPlanFromExplain(explain), "GEO_NEAR_2DSPHERE");
        assert.neq(null, geoNear, "expected a GEO_NEAR_2DSPHERE stage", {explain});
        assert.docEq({k: {$eq: 1}}, geoNear.filter, "unexpected filter", {geoNear});
    });

    it("does not report a filter when there is no residual predicate", function () {
        const explain = explainGeoNear({});
        const geoNear = getPlanStage(getWinningPlanFromExplain(explain), "GEO_NEAR_2DSPHERE");
        assert.neq(null, geoNear, "expected a GEO_NEAR_2DSPHERE stage", {explain});
        assert(!geoNear.hasOwnProperty("filter"), "unexpected filter", {geoNear});
    });

    it("counts documents rejected by the predicate as examined but not returned", function () {
        const explain = explainGeoNear({k: 1}, "executionStats");
        // The pipeline consists only of the $geoNear stage, so it is fully pushed down to the query
        // layer and the stats live under the cursor stage.
        const executionStats = explain.stages[0].$geoNearCursor.executionStats;
        assert.eq(10, executionStats.nReturned, "unexpected nReturned", {explain});
        // Every document in the annulus is fetched inside the near stage, including the ones the
        // predicate rejects.
        assert.eq(20, executionStats.totalDocsExamined, "unexpected totalDocsExamined", {explain});
        const geoNear = getPlanStage(executionStats.executionStages, "GEO_NEAR_2DSPHERE");
        assert.neq(null, geoNear, "expected a GEO_NEAR_2DSPHERE stage", {explain});
        assert.eq(10, geoNear.nReturned, "unexpected nReturned for the near stage", {geoNear});
    });

    it("returns correct results for a $where predicate", function () {
        // $where is not allowed inside an aggregation, so this goes through find.
        const res = coll
            .find(
                {
                    loc: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}},
                    $where: "this.k === 1",
                },
                {_id: 1},
            )
            .toArray();
        assert.eq(10, res.length, "unexpected results", {res});
        assert(
            res.every((doc) => doc._id % 2 === 1),
            "unexpected results",
            {res},
        );
    });

    it("still pushes the residual predicate into GEO_NEAR when find supplies a projection", function () {
        // Projection is applied above the $geoNear node, after the residual predicate has already
        // been pushed into the stage's docFilter. The predicate must still live inside GEO_NEAR,
        // not in a FETCH above it.
        const explain = coll
            .explain()
            .find(
                {
                    loc: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}},
                    k: 1,
                },
                {_id: 1},
            )
            .finish();
        const winningPlan = getWinningPlanFromExplain(explain);
        const geoNear = getPlanStage(winningPlan, "GEO_NEAR_2DSPHERE");
        assert.neq(null, geoNear, "expected a GEO_NEAR_2DSPHERE stage", {explain});
        assert.docEq({k: {$eq: 1}}, geoNear.filter, "unexpected filter", {geoNear});
    });

    it("does not push a trailing $match into GEO_NEAR", function () {
        // When the filter comes from a $match stage after $geoNear (rather than from the 'query'
        // parameter of $geoNear itself), the pipeline optimizer does not merge it into the geo
        // stage's query. The $match stays in the pipeline as a separate stage, so the GEO_NEAR
        // stage should not report it in its filter.
        const explain = coll.explain("executionStats").aggregate([
            {
                $geoNear: {
                    near: {type: "Point", coordinates: [0, 0]},
                    distanceField: "dist",
                },
            },
            {$match: {k: {$gte: 2}}},
        ]);
        const executionStats = explain.stages[0].$geoNearCursor.executionStats;
        const geoNear = getPlanStage(executionStats.executionStages, "GEO_NEAR_2DSPHERE");
        assert.neq(null, geoNear, "expected a GEO_NEAR_2DSPHERE stage", {explain});
        assert(
            !geoNear.hasOwnProperty("filter"),
            "a trailing $match was unexpectedly pushed into the GEO_NEAR stage",
            {geoNear},
        );
    });

    it("keeps the pushed-down predicate when the plan comes from the plan cache", function () {
        // Two candidate indexes force multiplanning, after which the winning plan is cached and
        // reused. The residual predicate is rebuilt from the cached solution, so it must still be
        // applied.
        const pipeline = [
            {
                $geoNear: {
                    near: {type: "Point", coordinates: [0, 0]},
                    distanceField: "dist",
                    query: {k: 1},
                },
            },
            {$project: {_id: 1}},
        ];
        const expected = coll.aggregate(pipeline).toArray();
        assert.eq(10, expected.length, "unexpected results", {expected});
        for (let i = 0; i < 3; ++i) {
            assert.eq(expected, coll.aggregate(pipeline).toArray(), "results changed on rerun");
        }
    });
});
