/**
 * Verifies the parallel planner output in explain.
 *
 * @tags: [
 * requires_sbe,
 * ]
 */

import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";

describe("explain of the parallel planner", function () {
    let conn;
    let db;
    let coll;
    // The engine the server picks on its own, restored after the case that forces classic.
    let defaultFrameworkControl;

    const filter = {a: {$gte: 0}};

    function setParams(params) {
        assert.commandWorked(db.adminCommand({setParameter: 1, ...params}));
    }

    function parallelPlanner(cursor) {
        return cursor.explain().queryPlanner.parallelPlanner;
    }

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagSbeFull: true,
                internalQueryEnableParallelQuery: true,
                internalQueryMaxDegreeOfParallelism: 4,
                internalQueryParallelQueryMinCollectionSizeBytes: 0,
            },
        });
        db = conn.getDB(jsTestName());
        coll = db.coll;
        assert.commandWorked(coll.insert([{a: 1}, {a: 2}]));
        defaultFrameworkControl = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
    });

    afterEach(function () {
        setParams({
            internalQueryEnableParallelQuery: true,
            internalQueryMaxDegreeOfParallelism: 4,
            internalQueryParallelQueryMinCollectionSizeBytes: 0,
            internalQueryFrameworkControl: defaultFrameworkControl,
        });
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("omits the section when the parallel planner never runs", function () {
        // The parallel planner only runs on the SBE lowering path, so a query the classic engine
        // plans has no decision to report at all.
        setParams({internalQueryFrameworkControl: "forceClassicEngine"});
        const explain = coll.find(filter).explain();
        assert(
            !explain.queryPlanner.hasOwnProperty("parallelPlanner"),
            "expected no parallelPlanner section under the classic engine",
            {explain},
        );
    });

    it("omits the section when parallel query is disabled", function () {
        // TODO SERVER-139183 Report the section unconditionally, as a queryKnob decision here.
        setParams({internalQueryEnableParallelQuery: false});
        const explain = coll.find(filter).explain();
        assert(
            !explain.queryPlanner.hasOwnProperty("parallelPlanner"),
            "expected no parallelPlanner section while parallel query is disabled",
            {explain},
        );
    });

    describe("when the heuristic gates fail", function () {
        it("reports a collection below the minimum size", function () {
            setParams({internalQueryParallelQueryMinCollectionSizeBytes: 1024 * 1024 * 1024});
            assert.docEq(
                {decision: "serial", reason: "collectionSize"},
                parallelPlanner(coll.find(filter)),
            );
        });

        it("reports a maximum degree of parallelism of one", function () {
            setParams({internalQueryMaxDegreeOfParallelism: 1});
            assert.docEq(
                {decision: "serial", reason: "queryKnob"},
                parallelPlanner(coll.find(filter)),
            );
        });
    });

    describe("when there is no viable parallel plan", function () {
        it("reports a $natural hint", function () {
            assert.docEq(
                {decision: "serial", reason: "requestedNaturalOrder"},
                parallelPlanner(coll.find(filter).hint({$natural: 1})),
            );
        });

        it("reports a $natural sort", function () {
            assert.docEq(
                {decision: "serial", reason: "requestedNaturalOrder"},
                parallelPlanner(coll.find(filter).sort({$natural: 1})),
            );
        });

        it("reports a plan node that is not parallel eligible", function () {
            assert.docEq(
                {decision: "serial", reason: "unsupportedNode"},
                parallelPlanner(coll.find(filter).limit(1)),
            );
        });

        it("reports a pushed down pipeline stage", function () {
            // TODO SERVER-138512 Support for plans with extended stages (group).
            const explain = coll.explain().aggregate([{$group: {_id: "$a", n: {$sum: 1}}}]);
            assert.docEq(
                {decision: "serial", reason: "unsupportedNode"},
                explain.queryPlanner.parallelPlanner,
            );
        });
    });

    describe("when a parallel plan is built", function () {
        it("shows the parallel plan and chooses parallel collscan if all heuristic gates pass", function () {
            const explain = coll.find(filter).explain();
            const planner = explain.queryPlanner.parallelPlanner;
            assert.eq("parallel", planner.decision, "unexpected decision", {planner});
            assert.eq("passedHeuristicGates", planner.reason, "unexpected reason", {planner});

            assert.eq("EXCHANGE", planner.parallelPlan.stage, "unexpected root stage", {planner});
            assert.eq("gather", planner.parallelPlan.routing, "unexpected routing", {planner});
            assert.eq(false, planner.parallelPlan.sortPreserving, "unexpected order", {planner});
            assert.docEq(
                {type: "unpartitioned"},
                planner.parallelPlan.distribution,
                "unexpected distribution",
            );
            assert.docEq(
                {type: "roundRobin"},
                planner.parallelPlan.inputStage.distribution,
                "unexpected input distribution",
            );
            assert.eq("COLLSCAN", planner.parallelPlan.inputStage.stage, "unexpected input", {
                planner,
            });
        });
    });
});
