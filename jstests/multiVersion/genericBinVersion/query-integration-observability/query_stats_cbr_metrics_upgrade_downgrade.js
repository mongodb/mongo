/**
 * Verifies that CBR metrics in query stats behave correctly in FCV upgrade/downgrade scenarios.
 * Since featureFlagQueryStatsCBRMetrics is FCV-gated, we expect CBR metrics to only appear
 * when fully upgraded.
 *
 * @tags: [requires_fcv_83]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {getQueryPlannerMetrics, getQueryStats, resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

const collName = jsTestName();
const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());

function runCBRQuery(db) {
    // Query with multiple indexes to trigger multiplanning/CBR.
    return db[collName].find({a: {$lt: 50}}).toArray();
}

/**
 * Asserts that CBR metrics appear in query stats output.
 * When the feature flag is enabled, the costBasedRanker section and planningTimeMicros
 * should be present under query planner metrics. i.e.:
 * { ....,
 *   planningTimeMicros: { sum: <positive>, ... },
 *   costBasedRanker: {
 *      nDocsSampled: { sum: <0 or positive>, ... },
 *      cardinalityEstimationMethods: {
 *          Histogram: <0 or positive>,
 *          Sampling: <0 or positive>,
 *          Heuristics: <0 or positive>,
 *          Mixed: <0 or positive>,
 *          Metadata: <0 or positive>,
 *          Code: <0 or positive>
 *      }
 *   }
 * }
 */
function assertCBRMetricsAppear(primaryConn) {
    const db = getDB(primaryConn);

    // Reset query stats store before running the query.
    resetQueryStatsStore(primaryConn, "1MB");

    jsTest.log.info("Running CBR query to check if metrics appear...");
    runCBRQuery(db);

    const stats = getQueryStats(primaryConn, {collName: collName});
    assert.eq(1, stats.length, `Expected 1 query stats entry: ${tojson(stats)}`);

    const queryPlannerSection = getQueryPlannerMetrics(stats[0].metrics);

    assert(
        queryPlannerSection.hasOwnProperty("planningTimeMicros"),
        `Expected planningTimeMicros in queryPlanner section: ${tojson(queryPlannerSection)}`,
    );
    assert.gt(
        queryPlannerSection.planningTimeMicros.sum,
        0,
        `planningTimeMicros.sum should be positive: ${tojson(queryPlannerSection.planningTimeMicros)}`,
    );

    assert(
        queryPlannerSection.hasOwnProperty("costBasedRanker"),
        `costBasedRanker section should be present when CBR is used: ${tojson(queryPlannerSection)}`,
    );
    const cbrSection = queryPlannerSection.costBasedRanker;

    assert(
        cbrSection.hasOwnProperty("nDocsSampled"),
        `nDocsSampled should be present in costBasedRanker section: ${tojson(cbrSection)}`,
    );
    assert.gte(
        cbrSection.nDocsSampled.sum,
        0,
        `nDocsSampled.sum should be 0 or positive: ${tojson(cbrSection.nDocsSampled)}`,
    );
    assert(
        cbrSection.hasOwnProperty("cardinalityEstimationMethods"),
        `cardinalityEstimationMethods should be present: ${tojson(cbrSection)}`,
    );
    const cardinalityEstimationMethods = cbrSection.cardinalityEstimationMethods;
    for (const method of ["Histogram", "Sampling", "Heuristics", "Mixed", "Metadata", "Code"]) {
        assert(
            cardinalityEstimationMethods.hasOwnProperty(method),
            `cardinalityEstimationMethods should have ${method} field: ${tojson(cbrSection)}`,
        );

        assert.gte(
            cardinalityEstimationMethods[method],
            0,
            `cardinalityEstimationMethods.${method} should be 0 or positive: ${tojson(cbrSection)}`,
        );
    }

    jsTest.log.info("CBR metrics appear correctly in query stats.");
}

/**
 * Asserts that CBR metrics do NOT appear in query stats output.
 * When the feature flag is not enabled, the costBasedRanker section and planningTimeMicros
 * should not be present.
 */
function assertCBRMetricsDoNotAppear(primaryConn) {
    const db = getDB(primaryConn);

    resetQueryStatsStore(primaryConn, "1MB");

    jsTest.log.info("Running CBR query to check if metrics do NOT appear...");
    runCBRQuery(db);

    const stats = getQueryStats(primaryConn, {collName: collName});
    assert.eq(1, stats.length, `Expected 1 query stats entry: ${tojson(stats)}`);

    const queryPlannerSection = getQueryPlannerMetrics(stats[0].metrics);

    assert(
        !queryPlannerSection.hasOwnProperty("planningTimeMicros"),
        `planningTimeMicros should NOT be present when CBR metrics feature is disabled: ${tojson(queryPlannerSection)}`,
    );
    assert(
        !queryPlannerSection.hasOwnProperty("costBasedRanker"),
        `costBasedRanker section should NOT be present when CBR metrics feature is disabled: ${tojson(queryPlannerSection)}`,
    );

    jsTest.log.info("CBR metrics correctly do NOT appear in query stats.");
}

/**
 * Sets up the collection with test data and indexes that will be used for multiplanning/CBR queries.
 */
const docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({a: i, b: i % 10, c: i % 5});
}

function setupCollection(primaryConnection, shardingTest = null) {
    const db = getDB(primaryConnection);
    const coll = assertDropAndRecreateCollection(db, collName);

    assert.commandWorked(coll.insertMany(docs));

    // Create indexes for multiplanning scenarios.
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assert.commandWorked(coll.createIndex({c: 1}));

    // Shard the collection if running on a sharded cluster.
    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1} /*key*/, {_id: docs.length / 2} /*split*/);
    }
}

describe("CBR metrics should only appear after cluster is fully upgraded.", function () {
    it("should only appear in a fully upgraded replica set", function () {
        testPerformUpgradeReplSet({
            startingNodeOptions: {setParameter: {internalQueryStatsRateLimit: -1}},
            upgradeNodeOptions: {setParameter: {internalQueryStatsRateLimit: -1}},
            setupFn: setupCollection,
            whenFullyDowngraded: assertCBRMetricsDoNotAppear,
            whenSecondariesAreLatestBinary: assertCBRMetricsDoNotAppear,
            whenBinariesAreLatestAndFCVIsLastLTS: assertCBRMetricsDoNotAppear,
            whenFullyUpgraded: assertCBRMetricsAppear,
        });
    });

    it("should only appear in a fully binary upgraded sharded cluster", function () {
        testPerformUpgradeSharded({
            startingNodeOptions: {setParameter: {internalQueryStatsRateLimit: -1}},
            upgradeNodeOptions: {setParameter: {internalQueryStatsRateLimit: -1}},
            setupFn: setupCollection,
            whenFullyDowngraded: assertCBRMetricsDoNotAppear,
            whenOnlyConfigIsLatestBinary: assertCBRMetricsDoNotAppear,
            whenSecondariesAndConfigAreLatestBinary: assertCBRMetricsDoNotAppear,
            whenMongosBinaryIsLastLTS: assertCBRMetricsDoNotAppear,
            whenBinariesAreLatestAndFCVIsLastLTS: assertCBRMetricsAppear,
            whenFullyUpgraded: assertCBRMetricsAppear,
        });
    });
});
