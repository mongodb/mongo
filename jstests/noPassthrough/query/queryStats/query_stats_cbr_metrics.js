/**
 * Tests that cost-based ranker metrics (e.g. planningTimeMicros) are collected in query stats.
 *
 * @tags: [
 *   featureFlagQueryStatsCBRMetrics,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {getQueryPlannerMetrics, getQueryStats, resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "testColl";

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
function runPlanningTimeMicrosTests(topologyName, setupFn, teardownFn) {
    describe(`planningTimeMicros in query stats (${topologyName})`, function () {
        let fixture;
        let conn;
        let testDB;
        let coll;

        before(function () {
            const setupRes = setupFn();
            fixture = setupRes.fixture;
            conn = setupRes.conn;
            testDB = setupRes.testDB;
            coll = testDB[collName];
            coll.drop();

            // Insert documents and create indexes for multiplanning scenarios.
            for (let i = 0; i < 100; i++) {
                assert.commandWorked(coll.insert({a: i, b: i % 10, c: i % 5}));
            }
            assert.commandWorked(coll.createIndex({a: 1}));
            assert.commandWorked(coll.createIndex({b: 1}));
            assert.commandWorked(coll.createIndex({c: 1}));
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
        });

        it("should capture planningTimeMicros with multiplanning", function () {
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

            failPoints.forEach((failPoint) => failPoint.off());
        });
    });
}

runPlanningTimeMicrosTests(
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

runPlanningTimeMicrosTests(
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
