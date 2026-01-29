/**
 * Tests that cost-based ranker (CBR) metrics (e.g. planningTimeMicros,
 * cardinalityEstimationMethods) are collected in query stats.
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {getQueryPlannerMetrics, getQueryStats, resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "testColl";
const automaticCECollName = "automaticCETestColl";

/**
 * Verifies that planningTimeMicros is present, in the correct location,
 * has positive values, and is less than total execution time.
 */
function validatePlanningTimeMicros(metrics) {
    const queryPlannerSection = getQueryPlannerMetrics(metrics);

    assert(
        queryPlannerSection.hasOwnProperty("planningTimeMicros"),
        `Expected planningTimeMicros in queryPlanner section: ${tojson(queryPlannerSection)}`,
    );

    const metric = queryPlannerSection.planningTimeMicros;

    assert.gt(metric.sum, 0, `planningTimeMicros.sum should be positive: ${tojson(metric)}`);
    assert.gt(metric.min, 0, `planningTimeMicros.min should be positive: ${tojson(metric)}`);
    assert.gt(metric.max, 0, `planningTimeMicros.max should be positive: ${tojson(metric)}`);

    const totalExecTime = Number(metrics.totalExecMicros?.sum || metrics.cursor?.firstResponseExecMicros?.sum || 0);
    assert.gt(
        totalExecTime,
        0,
        `Expected totalExecMicros or cursor.firstResponseExecMicros to be present: ${tojson(metrics)}`,
    );
    assert.lt(
        Number(metric.sum),
        totalExecTime,
        `planningTimeMicros (${metric.sum}) should be < totalExecMicros (${totalExecTime})`,
    );
}

/**
 * Runs the test suite against a specific topology.
 */
function runCBRMetricsTests(topologyName, setupFn, teardownFn) {
    describe(`CBR metrics in query stats (${topologyName})`, function () {
        let fixture;
        let conn;
        let testDB;
        let coll;
        let automaticCEColl;
        let isSbeEnabled;

        before(function () {
            const setupRes = setupFn();
            fixture = setupRes.fixture;
            conn = setupRes.conn;
            testDB = setupRes.testDB;
            coll = testDB[collName];
            coll.drop();

            // TODO SERVER-117707 Remove once CBR enabled with SBE.
            isSbeEnabled = checkSbeFullyEnabled(testDB);

            // Insert documents and create indexes for multiplanning scenarios.
            for (let i = 0; i < 100; i++) {
                assert.commandWorked(coll.insert({a: i, b: i % 10, c: i % 5}));
            }
            assert.commandWorked(coll.createIndex({a: 1}));
            assert.commandWorked(coll.createIndex({b: 1}));
            assert.commandWorked(coll.createIndex({c: 1}));

            // Setup collection for automaticCE tests (pattern from cbr_plan_cache.js).
            automaticCEColl = testDB[automaticCECollName];
            automaticCEColl.drop();
            const docs = [];
            const kNumDocs = 15000;
            for (let i = 0; i < kNumDocs; i++) {
                docs.push({a: i, b: i});
            }
            docs.push({a: 7001, b: 7001, c: 1});
            docs.push({a: 8001, b: 8001, c: 1});
            assert.commandWorked(automaticCEColl.insertMany(docs));
            assert.commandWorked(automaticCEColl.createIndexes([{a: 1}, {b: 1}]));
        });

        after(function () {
            if (fixture) {
                teardownFn(fixture);
            }
        });

        beforeEach(function () {
            resetQueryStatsStore(conn, "1MB");
        });

        it("should have planningTimeMicros with plan cache", function () {
            // Using the plan cache requires 3 runs:
            //   1st run: creates an inactive cache entry
            //   2nd run: activates the cache entry
            //   3rd run: uses the plan cache
            coll.find({a: {$gt: 50}, b: {$lt: 5}}).toArray();
            coll.find({a: {$gt: 50}, b: {$lt: 5}}).toArray();
            coll.find({a: {$gt: 50}, b: {$lt: 5}}).toArray();

            const stats = getQueryStats(conn, {collName: collName});
            assert.eq(1, stats.length, `Expected 1 query stats entry: ${tojson(stats)}`);

            const queryPlannerSection = getQueryPlannerMetrics(stats[0].metrics);

            // Verify plan cache was used on the third execution.
            assert.gt(
                Number(queryPlannerSection.fromPlanCache?.["true"] || 0),
                0,
                `Expected fromPlanCache.true > 0: ${tojson(queryPlannerSection)}`,
            );

            validatePlanningTimeMicros(stats[0].metrics);

            // The costBasedRanker section should be present with 0 nDocsSampled and all-zero
            // cardinalityEstimationMethods.
            assert(
                queryPlannerSection.hasOwnProperty("costBasedRanker"),
                `costBasedRanker section should be present: ${tojson(queryPlannerSection)}`,
            );
            const cbrSection = queryPlannerSection.costBasedRanker;
            assert(
                cbrSection.hasOwnProperty("cardinalityEstimationMethods"),
                `cardinalityEstimationMethods should be present: ${tojson(cbrSection)}`,
            );
            assert.eq(
                cbrSection.cardinalityEstimationMethods,
                {
                    Histogram: NumberLong(0),
                    Sampling: NumberLong(0),
                    Heuristics: NumberLong(0),
                    Mixed: NumberLong(0),
                    Metadata: NumberLong(0),
                    Code: NumberLong(0),
                },
                `cardinalityEstimationMethods should be all zeros when CBR not used: ${tojson(cbrSection)}`,
            );
            assert.eq(
                0,
                cbrSection.nDocsSampled.sum,
                `nDocsSampled should be empty when CBR not used: ${tojson(cbrSection)}`,
            );
        });

        it("should capture planningTimeMicros and no CBR metrics when only multiplanning is used", function () {
            // Use a failpoint to inject a delay during multiplanning, guaranteeing a floor time
            // for planningTimeMicros. The 100ms value ensures that planningTime dominates total
            // query execution time.
            const waitTimeMillis = 100;

            // Configure failpoint on all nodes.
            const failPoints = FixtureHelpers.mapOnEachShardNode({
                db: testDB.getSiblingDB("admin"),
                func: (db) => configureFailPoint(db, "sleepWhileMultiplanning", {ms: waitTimeMillis}),
                primaryNodeOnly: true,
            });

            // Use a different filter shape than the previous test to ensure multiplanning.
            coll.find({b: {$gt: 2}, c: {$lt: 3}}).toArray();

            const stats = getQueryStats(conn, {collName: collName});
            assert.eq(1, stats.length, `Expected 1 query stats entry: ${tojson(stats)}`);

            const queryPlannerSection = getQueryPlannerMetrics(stats[0].metrics);

            // Verify multiplanning was used.
            assert.gt(
                Number(queryPlannerSection.fromMultiPlanner?.["true"] || 0),
                0,
                `Expected fromMultiPlanner.true > 0: ${tojson(queryPlannerSection)}`,
            );

            // Validate planningTimeMicros is within bounds:
            // waitTime < planningTime < totalExecutionTime
            validatePlanningTimeMicros(stats[0].metrics);

            // Convert planningTimeMicros to milliseconds for comparison with failpoint delay.
            const planningTimeMillis = Number(queryPlannerSection.planningTimeMicros.sum) / 1000;
            assert.gt(
                planningTimeMillis,
                waitTimeMillis,
                `planningTime (${planningTimeMillis}ms) should be >= failpoint delay (${waitTimeMillis}ms)`,
            );

            // The costBasedRanker section should be present with 0 nDocsSampled and all-zero
            // cardinalityEstimationMethods when only multiplanning was used.
            assert(
                queryPlannerSection.hasOwnProperty("costBasedRanker"),
                `costBasedRanker section should be present: ${tojson(queryPlannerSection)}`,
            );
            const cbrSection = queryPlannerSection.costBasedRanker;
            assert(
                cbrSection.hasOwnProperty("cardinalityEstimationMethods"),
                `cardinalityEstimationMethods should be present: ${tojson(cbrSection)}`,
            );
            assert.eq(
                cbrSection.cardinalityEstimationMethods,
                {
                    Histogram: NumberLong(0),
                    Sampling: NumberLong(0),
                    Heuristics: NumberLong(0),
                    Mixed: NumberLong(0),
                    Metadata: NumberLong(0),
                    Code: NumberLong(0),
                },
                `cardinalityEstimationMethods should be all zeros when CBR not used: ${tojson(cbrSection)}`,
            );
            assert.eq(
                0,
                cbrSection.nDocsSampled.sum,
                `nDocsSampled should be empty when CBR not used: ${tojson(cbrSection)}`,
            );

            failPoints.forEach((failPoint) => failPoint.off());
        });

        it("should have costBasedRanker section with nDocsSampled when CBR uses sampling to pick the best plan", function () {
            // Choose maximum allowed margin of error (10%) and 95% CI for predictability.
            // This should result in a sample size of 96 documents.
            const samplingMarginOfError = 10.0;
            const confidenceInterval = "95";
            const zScore = 1.96; // Z-score for 95% confidence interval.

            let previousPlanRankerMode;
            let previousSequentialScanFlag;
            let previousMarginOfError;
            let previousConfidenceInterval;
            try {
                FixtureHelpers.mapOnEachShardNode({
                    db: testDB.getSiblingDB("admin"),
                    func: (db) => {
                        previousPlanRankerMode = assert.commandWorked(
                            db.adminCommand({setParameter: 1, planRankerMode: "samplingCE"}),
                        ).was;
                        // Use sequential scan to make sampled documents deterministic for the assertion.
                        previousSequentialScanFlag = assert.commandWorked(
                            db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}),
                        ).was;
                        previousMarginOfError = assert.commandWorked(
                            db.adminCommand({setParameter: 1, samplingMarginOfError: samplingMarginOfError}),
                        ).was;
                        previousConfidenceInterval = assert.commandWorked(
                            db.adminCommand({setParameter: 1, samplingConfidenceInterval: confidenceInterval}),
                        ).was;
                    },
                    primaryNodeOnly: true,
                });

                // Execute a query that will trigger the cost-based ranker sampling CE path.
                coll.find({}).toArray();

                // Verify that the nDocsSampled metric matches the expected sample size for the given margin of error and confidence interval.
                const stats = getQueryStats(conn, {collName: collName});
                assert.eq(1, stats.length, `Expected 1 query stats entry: ${tojson(stats)}`);
                const queryPlannerSection = getQueryPlannerMetrics(stats[0].metrics);
                assert(
                    queryPlannerSection.hasOwnProperty("costBasedRanker"),
                    `costBasedRanker section should be present when CBR is used: ${tojson(queryPlannerSection)}`,
                );
                const cbrSection = queryPlannerSection.costBasedRanker;

                // TODO SERVER-117707 Remove branch.
                if (isSbeEnabled) {
                    assert.eq(cbrSection.nDocsSampled.sum, 0);
                } else {
                    const expectedSampleSize = Math.round(zScore ** 2 / ((2 * samplingMarginOfError) / 100.0) ** 2);
                    const nDocsSampled = Number(cbrSection.nDocsSampled.sum);
                    assert.eq(
                        nDocsSampled,
                        expectedSampleSize,
                        `Expected nDocsSampled == ${expectedSampleSize} (CI=${
                            confidenceInterval
                        }%, MoE=${samplingMarginOfError}%) but got ${tojson(cbrSection)}`,
                    );
                }
            } finally {
                // Reset knobs to defaults on all nodes.
                FixtureHelpers.mapOnEachShardNode({
                    db: testDB.getSiblingDB("admin"),
                    func: (db) => {
                        assert.commandWorked(
                            db.adminCommand({setParameter: 1, planRankerMode: previousPlanRankerMode}),
                        );
                        assert.commandWorked(
                            db.adminCommand({
                                setParameter: 1,
                                internalQuerySamplingBySequentialScan: previousSequentialScanFlag,
                            }),
                        );
                        assert.commandWorked(
                            db.adminCommand({setParameter: 1, samplingMarginOfError: previousMarginOfError}),
                        );
                        assert.commandWorked(
                            db.adminCommand({
                                setParameter: 1,
                                samplingConfidenceInterval: previousConfidenceInterval,
                            }),
                        );
                    },
                    primaryNodeOnly: true,
                });
            }
        });

        it("should have populated CBR metrics with samplingCE", function () {
            let prevPlanRankerMode;
            try {
                FixtureHelpers.mapOnEachShardNode({
                    db: testDB.getSiblingDB("admin"),
                    func: (db) => {
                        prevPlanRankerMode = assert.commandWorked(
                            db.adminCommand({setParameter: 1, planRankerMode: "samplingCE"}),
                        ).was;
                    },
                    primaryNodeOnly: true,
                });

                coll.find({a: {$lt: 50}}).toArray();

                const stats = getQueryStats(conn, {collName: collName});
                assert.eq(1, stats.length, `Expected 1 query stats entry: ${tojson(stats)}`);
                const queryPlannerSection = getQueryPlannerMetrics(stats[0].metrics);
                assert(
                    queryPlannerSection.hasOwnProperty("costBasedRanker"),
                    `costBasedRanker section should be present when CBR is used: ${tojson(queryPlannerSection)}`,
                );
                const cbrSection = queryPlannerSection.costBasedRanker;

                // TODO SERVER-117707 Remove branch.
                if (isSbeEnabled) {
                    assert.eq(cbrSection.cardinalityEstimationMethods, {
                        Histogram: NumberLong(0),
                        Sampling: NumberLong(0),
                        Heuristics: NumberLong(0),
                        Mixed: NumberLong(0),
                        Metadata: NumberLong(0),
                        Code: NumberLong(0),
                    });
                } else {
                    assert.eq(cbrSection.cardinalityEstimationMethods, {
                        Histogram: NumberLong(0),
                        Sampling: NumberLong(1),
                        Heuristics: NumberLong(0),
                        Mixed: NumberLong(0),
                        Metadata: NumberLong(0),
                        Code: NumberLong(0),
                    });
                }
            } finally {
                // Reset knobs to defaults on all nodes.
                FixtureHelpers.mapOnEachShardNode({
                    db: testDB.getSiblingDB("admin"),
                    func: (db) => {
                        assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: prevPlanRankerMode}));
                    },
                    primaryNodeOnly: true,
                });
            }
        });

        it("should have no CBR metrics with automaticCE+fallback when we do not hit the CBR fallback", function () {
            let prevPlanRankerMode;
            let prevAutomaticCEPlanRankingStrategy;
            try {
                FixtureHelpers.mapOnEachShardNode({
                    db: testDB.getSiblingDB("admin"),
                    func: (db) => {
                        prevPlanRankerMode = assert.commandWorked(
                            db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"}),
                        ).was;
                        prevAutomaticCEPlanRankingStrategy = assert.commandWorked(
                            db.adminCommand({
                                setParameter: 1,
                                automaticCEPlanRankingStrategy: "CBRForNoMultiplanningResults",
                            }),
                        ).was;
                    },
                    primaryNodeOnly: true,
                });

                // We do not expect this query to hit the CBR fallback.
                const bIndexQuery = {a: {$gte: 1}, b: {$gte: 14500}, c: 1};
                automaticCEColl.find(bIndexQuery).toArray();

                const stats = getQueryStats(conn, {collName: automaticCECollName});
                assert.eq(1, stats.length, `Expected 1 query stats entry: ${tojson(stats)}`);

                const queryPlannerSection = getQueryPlannerMetrics(stats[0].metrics);
                assert(
                    queryPlannerSection.hasOwnProperty("costBasedRanker"),
                    `costBasedRanker section should be present: ${tojson(queryPlannerSection)}`,
                );
                const cbrSection = queryPlannerSection.costBasedRanker;
                assert.eq(
                    cbrSection.cardinalityEstimationMethods,
                    {
                        Histogram: NumberLong(0),
                        Sampling: NumberLong(0),
                        Heuristics: NumberLong(0),
                        Mixed: NumberLong(0),
                        Metadata: NumberLong(0),
                        Code: NumberLong(0),
                    },
                    `cardinalityEstimationMethods should be all zeros when CBR not used: ${tojson(cbrSection)}`,
                );
                assert.eq(
                    0,
                    cbrSection.nDocsSampled.sum,
                    `nDocsSampled should be empty when CBR not used: ${tojson(cbrSection)}`,
                );
            } finally {
                // Reset knobs to defaults on all nodes.
                FixtureHelpers.mapOnEachShardNode({
                    db: testDB.getSiblingDB("admin"),
                    func: (db) => {
                        assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: prevPlanRankerMode}));
                        assert.commandWorked(
                            db.adminCommand({
                                setParameter: 1,
                                automaticCEPlanRankingStrategy: prevAutomaticCEPlanRankingStrategy,
                            }),
                        );
                    },
                    primaryNodeOnly: true,
                });
            }
        });

        it("should have populated CBR metrics with automaticCE+fallback when we do hit the CBR fallback", function () {
            let prevPlanRankerMode;
            let prevAutomaticCEPlanRankingStrategy;
            try {
                FixtureHelpers.mapOnEachShardNode({
                    db: testDB.getSiblingDB("admin"),
                    func: (db) => {
                        prevPlanRankerMode = assert.commandWorked(
                            db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"}),
                        ).was;
                        prevAutomaticCEPlanRankingStrategy = assert.commandWorked(
                            db.adminCommand({
                                setParameter: 1,
                                automaticCEPlanRankingStrategy: "CBRForNoMultiplanningResults",
                            }),
                        ).was;
                    },
                    primaryNodeOnly: true,
                });

                // We expect this query to hit the CBR fallback.
                const aIndexQuery = {a: {$gte: 1}, b: {$gte: 2}, c: 1};
                automaticCEColl.find(aIndexQuery).toArray();

                const stats = getQueryStats(conn, {collName: automaticCECollName});
                assert.eq(1, stats.length, `Expected 1 query stats entry: ${tojson(stats)}`);

                const queryPlannerSection = getQueryPlannerMetrics(stats[0].metrics);
                assert(
                    queryPlannerSection.hasOwnProperty("costBasedRanker"),
                    `costBasedRanker section should be present when CBR is used: ${tojson(queryPlannerSection)}`,
                );
                const cbrSection = queryPlannerSection.costBasedRanker;

                // TODO SERVER-117707 Remove branch.
                if (isSbeEnabled) {
                    assert.eq(cbrSection.cardinalityEstimationMethods, {
                        Histogram: NumberLong(0),
                        Sampling: NumberLong(0),
                        Heuristics: NumberLong(0),
                        Mixed: NumberLong(0),
                        Metadata: NumberLong(0),
                        Code: NumberLong(0),
                    });
                    assert.eq(cbrSection.nDocsSampled.sum, 0);
                } else {
                    assert.eq(cbrSection.cardinalityEstimationMethods, {
                        Histogram: NumberLong(0),
                        Sampling: NumberLong(1),
                        Heuristics: NumberLong(0),
                        Mixed: NumberLong(0),
                        Metadata: NumberLong(0),
                        Code: NumberLong(0),
                    });
                    assert.gt(
                        cbrSection.nDocsSampled.sum,
                        0,
                        `nDocsSampled should be positive when CBR is used: ${tojson(cbrSection)}`,
                    );
                }
            } finally {
                // Reset knobs to defaults on all nodes.
                FixtureHelpers.mapOnEachShardNode({
                    db: testDB.getSiblingDB("admin"),
                    func: (db) => {
                        assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: prevPlanRankerMode}));
                        assert.commandWorked(
                            db.adminCommand({
                                setParameter: 1,
                                automaticCEPlanRankingStrategy: prevAutomaticCEPlanRankingStrategy,
                            }),
                        );
                    },
                    primaryNodeOnly: true,
                });
            }
        });
    });
}

runCBRMetricsTests(
    "Standalone",
    () => {
        const conn = MongoRunner.runMongod({
            setParameter: {internalQueryStatsRateLimit: -1},
        });
        assert.neq(null, conn, "mongod was unable to start up");
        return {fixture: conn, conn: conn, testDB: conn.getDB(dbName)};
    },
    (fixture) => MongoRunner.stopMongod(fixture),
);

runCBRMetricsTests(
    "Sharded",
    () => {
        const st = new ShardingTest({
            shards: 2,
            mongosOptions: {setParameter: {internalQueryStatsRateLimit: -1}},
        });
        const testDB = st.s.getDB(dbName);
        return {fixture: st, conn: st.s, testDB: testDB};
    },
    (fixture) => fixture.stop(),
);
