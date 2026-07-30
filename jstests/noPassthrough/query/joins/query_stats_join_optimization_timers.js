/**
 * Validates that each join optimization timing metric reported in query stats reports a meaningful time.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryStats,
    getQueryStatsServerParameters,
} from "jstests/libs/query/query_stats_utils.js";

const kSleepMillis = 250;
const kSleepMicros = kSleepMillis * 1000;
const kTimerFailPoints = {
    joinModelingTimeMicros: "sleepWhileBuildingJoinModel",
    sbeLoweringTimeMicros: "sleepWhileLoweringJoinPlanToSbe",
    samplingTimeMicros: "sleepWhileSamplingForJoinOptimization",
    cbrPlanningTimeMicros: "sleepWhileCbrPlanningForJoinOptimization",
    planEnumerationTimeMicros: "sleepWhileEnumeratingJoinPlans",
    ceTimeMicros: "sleepWhileEstimatingJoinCardinality",
};

describe("join optimization timing metrics in query stats", function () {
    const params = getQueryStatsServerParameters();
    // Keep the join plan cache off so that every query runs the full optimization pipeline,
    // including the phases that are only reached on a cache miss.
    params.setParameter.internalEnableJoinPlanCache = false;
    params.setParameter.internalEnableJoinOptimization = true;
    const conn = MongoRunner.runMongod(params);
    assert.neq(null, conn, "mongod was unable to start up");

    const db = conn.getDB(jsTestName());

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    /**
     * (Re-)creates the collections for a three-node join and returns the base collection along with
     * the pipeline to run against it.
     */
    function setupCollections() {
        const colls = ["orders", "customers", "items"].map((name) => db[`${name}`]);
        for (const coll of colls) {
            coll.drop();
            const docs = [];
            for (let i = 0; i < 100; i++) {
                docs.push({_id: i, a: i, b: i % 10});
            }
            assert.commandWorked(coll.insertMany(docs));
            assert.commandWorked(coll.createIndex({a: 1, b: 1}));
        }
        const [orders, customers, items] = colls;
        const pipeline = [
            {
                $lookup: {
                    from: customers.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "customer",
                },
            },
            {$unwind: "$customer"},
            {$lookup: {from: items.getName(), localField: "b", foreignField: "b", as: "item"}},
            {$unwind: "$item"},
        ];
        return {orders, pipeline};
    }

    /**
     * Returns the JoinOptimization supplemental metrics for the join pipeline, or null if the query
     * has not run yet.
     */
    function getJoinMetrics(collName) {
        const stats = getQueryStats(conn, {collName});
        if (stats.length === 0) {
            return null;
        }
        assert.eq(1, stats.length, {stats});
        const joinMetrics = stats[0].metrics.supplementalMetrics.JoinOptimization;
        assert(joinMetrics, "expected JoinOptimization supplemental metrics", {stats});
        return joinMetrics;
    }

    /**
     * Runs the join pipeline once, enabling the fail point for 'timerName's phase, and returns how
     * many microseconds that single execution contributed to each timer.
     *
     * Every test runs the same query shape against the same collections, so all executions
     * aggregate into one query stats entry - 'min' and 'max' span every execution so far, and 'sum'
     * accumulates. We therefore snapshot the entry before the run and report the delta in 'sum', so
     * that each assertion sees only the time this execution contributed.
     */
    function runWithSleepInPhase(timerName) {
        const {orders, pipeline} = setupCollections();

        const failPointName = kTimerFailPoints[timerName];
        assert(failPointName, `no fail point known for timer '${timerName}'`);
        const before = getJoinMetrics(orders.getName());
        const fp = configureFailPoint(conn, failPointName, {ms: kSleepMillis});
        try {
            assert.eq(orders.aggregate(pipeline, {cursor: {batchSize: 100000}}).itcount(), 1000);
        } finally {
            fp.off();
        }
        const after = getJoinMetrics(orders.getName());
        assert(after, "expected a query stats entry after running the join pipeline");

        // Exactly one execution should have been aggregated in since the snapshot, otherwise the
        // deltas below would cover more than the run we just did.
        const priorUpdateCount = before ? before.updateCount : 0;
        assert.eq(after.updateCount, priorUpdateCount + 1, {before, after});

        const deltas = {};
        for (const name of Object.keys(kTimerFailPoints)) {
            assert(after[name], `missing timer '${name}'`, {after});
            deltas[name] = after[name].sum - (before ? before[name].sum : 0);
        }
        return deltas;
    }

    function assertTimerReflectsSleep(deltas, timerName) {
        assert.gte(deltas[timerName], kSleepMicros, `timer '${timerName}'`, {deltas});
    }

    function assertTimerUnaffected(deltas, timerName) {
        assert.lt(deltas[timerName], kSleepMicros, `timer '${timerName}'`, {deltas});
    }

    /**
     * Asserts that the injected delay showed up in every timer in 'affected' and in no other timer.
     * The negative half is what pins each timer to its own phase: without it a timer that measured
     * too much - say, all of join optimization - would still pass the positive check.
     */
    function assertSleepAffectsOnly(deltas, affected) {
        for (const timerName of Object.keys(kTimerFailPoints)) {
            if (affected.includes(timerName)) {
                assertTimerReflectsSleep(deltas, timerName);
            } else {
                assertTimerUnaffected(deltas, timerName);
            }
        }
    }

    it("joinModelingTimeMicros measures join model construction", function () {
        const deltas = runWithSleepInPhase("joinModelingTimeMicros");
        assertSleepAffectsOnly(deltas, ["joinModelingTimeMicros"]);
    });

    it("sbeLoweringTimeMicros measures lowering the chosen plan to SBE", function () {
        const deltas = runWithSleepInPhase("sbeLoweringTimeMicros");
        assertSleepAffectsOnly(deltas, ["sbeLoweringTimeMicros"]);
    });

    it("samplingTimeMicros measures building the sampling estimators", function () {
        const deltas = runWithSleepInPhase("samplingTimeMicros");
        assertSleepAffectsOnly(deltas, ["samplingTimeMicros"]);
    });

    it("cbrPlanningTimeMicros measures single table access planning", function () {
        const deltas = runWithSleepInPhase("cbrPlanningTimeMicros");
        assertSleepAffectsOnly(deltas, ["cbrPlanningTimeMicros"]);
    });

    it("planEnumerationTimeMicros measures plan enumeration", function () {
        const deltas = runWithSleepInPhase("planEnumerationTimeMicros");
        // The delay is injected before enumeration starts walking the lattice, so it does not land
        // inside any of the cardinality estimates that 'ceTimeMicros' charges for.
        assertSleepAffectsOnly(deltas, ["planEnumerationTimeMicros"]);
    });

    it("ceTimeMicros measures cardinality estimation", function () {
        const deltas = runWithSleepInPhase("ceTimeMicros");
        // Most cardinality estimation happens during plan enumeration, so the delay shows up in
        // that timer too. Note that the two are not strictly nested: some estimates (e.g. edge
        // selectivities) are computed while constructing the estimator, before enumeration starts.
        assertSleepAffectsOnly(deltas, ["ceTimeMicros", "planEnumerationTimeMicros"]);
    });
});
