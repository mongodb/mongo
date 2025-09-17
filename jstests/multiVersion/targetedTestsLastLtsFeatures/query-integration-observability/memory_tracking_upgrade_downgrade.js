/**
 * Verifies that memory tracking behaves correctly in FCV upgrade/downgrade scenarios.  We do not
 * test the profiler output here because the profiler can only be run on a standalone.
 *
 * TODO SERVER-88298 Remove when feature flag is enabled by default.
 * @tags: [featureFlagQueryMemoryTracking]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {runPipelineAndGetDiagnostics, verifySlowQueryLogMetrics} from "jstests/libs/query/memory_tracking_utils.js";
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";
import {checkCurrentOpMemoryTracking} from "jstests/noPassthrough/memory_tracking/current_op.js";

const collName = jsTestName();
const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());
const docs = [
    {_id: 0, name: "Espresso", price: 2.5, category: "Coffee"},
    {_id: 1, name: "Cappuccino", price: 4.25, category: "Coffee"},
    {_id: 2, name: "Latte", price: 4.75, category: "Coffee"},
    {_id: 3, name: "Americano", price: 3.0, category: "Coffee"},
    {_id: 4, name: "Sweet Potato Latte", price: 5.5, category: "Specialty"},
    {_id: 5, name: "Barley Tea", price: 2.0, category: "Tea"},
    {_id: 6, name: "Brown Butter Maple Latte", price: 6.75, category: "Specialty"},
    {_id: 7, name: "Coconut Lime Tea", price: 4.0, category: "Tea"},
    {_id: 8, name: "Turkish Coffee", price: 3.25, category: "Coffee"},
    {_id: 9, name: "Honey Deuce", price: 22.25, category: "Specialty"},
    {_id: 10, name: "Corn Latte", price: 7.0, category: "Specialty"},
    {_id: 11, name: "Strawberry Matcha", price: 100.0, category: "Tea"},
];

// Group by category and calculate average price and item count per category.
const pipeline = [
    {
        $group: {
            _id: "$category",
            avgPrice: {$avg: "$price"},
            itemCount: {$sum: 1},
            totalRevenue: {$sum: "$price"},
        },
    },
];

/**
 * Helpers for checking memory tracking metrics in logs and explain output.
 */
function getGroupStageName(db) {
    const getParam = db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1});
    if (getParam.internalQueryFrameworkControl.value != "forceClassicEngine") {
        return "group";
    } else {
        return "$group";
    }
}

function assertMemoryTrackingSlowQueryLogMetrics(db, expectMemoryMetrics) {
    const logLines = runPipelineAndGetDiagnostics({
        db: db,
        collName: collName,
        commandObj: {
            aggregate: collName,
            pipeline: pipeline,
            allowDiskUse: false,
            cursor: {batchSize: 1},
        },
        source: "log",
    });
    verifySlowQueryLogMetrics({
        logLines,
        verifyOptions: {expectedNumGetMores: 3, expectMemoryMetrics: expectMemoryMetrics},
    });
}

function assertMemoryTrackingExplainExecutionStatsMetrics(db, expectMemoryMetrics) {
    const explainRes = db[collName].explain("executionStats").aggregate(pipeline);
    const stages = getAggPlanStages(explainRes, getGroupStageName(db));
    assert.gt(stages.length, 0, "Expected at least one $group stage in: " + tojson(explainRes));

    for (let stage of stages) {
        if (expectMemoryMetrics) {
            assert(
                stage.hasOwnProperty("peakTrackedMemBytes"),
                `Expected peakTrackedMemBytes in $group stage: ` + tojson(explainRes),
            );
            assert.gt(
                stage.peakTrackedMemBytes,
                0,
                `Expected peakTrackedMemBytes to be positive in $group stage: ` + tojson(explainRes),
            );
        } else {
            assert(
                !stage.hasOwnProperty("peakTrackedMemBytes"),
                `Unexpected peakTrackedMemBytes in $group stage: ` + tojson(explainRes),
            );
        }
    }
}

/**
 * Sets server parameters for the test. We need to profile all operations and set forceClassicEngine
 * on each primary in the replica set/cluster.
 */
function setServerParams(db) {
    db.setProfilingLevel(0, {slowms: -1});
}

/**
 * Callbacks used for the upgrade tests.
 */

function setupCollection(primaryConnection, shardingTest = null) {
    const db = getDB(primaryConnection);
    const coll = assertDropAndRecreateCollection(db, collName);

    assert.commandWorked(coll.insertMany(docs));

    // Shard the collection to get even distribution across 2 shards.
    // shard 0: {_id: 0, ..., 5}
    // shard 1: {_id: 6, ..., 11}
    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1} /*key*/, {_id: docs.length / 2} /*split*/);

        const shardedDataDistribution = shardingTest.s
            .getDB("admin")
            .aggregate([{$shardedDataDistribution: {}}, {$match: {ns: coll.getFullName()}}])
            .toArray();
        assert.eq(shardedDataDistribution.length, 1);
        assert.eq(
            shardedDataDistribution[0].shards.length,
            2,
            "Expected 2 shards in data distribution" + tojson(shardedDataDistribution),
        );
        assert.eq(
            shardedDataDistribution[0].shards[0].numOwnedDocuments,
            docs.length / 2,
            `Expected ${docs.length / 2} docs on shard 0 in data distribution` + tojson(shardedDataDistribution),
        );
        assert.eq(
            shardedDataDistribution[0].shards[1].numOwnedDocuments,
            docs.length / 2,
            `Expected ${docs.length / 2} docs on shard 1 in data distribution` + tojson(shardedDataDistribution),
        );
    }
}

function assertMemoryTrackingMetricsAppear(primaryConn) {
    const db = getDB(primaryConn);
    setServerParams(db);

    jsTest.log.info("Checking logs for memory tracking stats...");
    assertMemoryTrackingSlowQueryLogMetrics(db, true /*expectMemoryMetrics*/);

    jsTest.log.info("Checking explain for memory tracking stats...");
    assertMemoryTrackingExplainExecutionStatsMetrics(db, true /*expectMemoryMetrics*/);

    jsTest.log.info("Checking $currentOp for memory tracking stats...");
    checkCurrentOpMemoryTracking("group", primaryConn, db, db[collName], pipeline, true /*expectMemoryMetrics*/);
}

function assertMemoryTrackingMetricsDoNotAppear(primaryConn) {
    const db = getDB(primaryConn);
    setServerParams(db);

    jsTest.log.info("Checking that logs do not have memory tracking stats...");
    assertMemoryTrackingSlowQueryLogMetrics(db, false /*expectMemoryMetrics*/);

    jsTest.log.info("Checking that explain does not memory tracking stats...");
    assertMemoryTrackingExplainExecutionStatsMetrics(db, false /*expectMemoryMetrics*/);

    jsTest.log.info("Checking that $currentOp does not have memory tracking stats...");
    checkCurrentOpMemoryTracking("group", primaryConn, db, db[collName], pipeline, false /*expectMemoryMetrics*/);
}

function assertMemoryTrackingMetricsAppearOnOneShard(primaryConn) {
    const db = getDB(primaryConn);
    setServerParams(db);

    jsTest.log.info("Checking that logs do not have memory tracking stats...");
    assertMemoryTrackingSlowQueryLogMetrics(db, false /*expectMemoryMetrics*/);

    jsTest.log.info("Checking that memory tracking stats appear on one shard for explain...");

    const explainRes = db[collName].explain("executionStats").aggregate(pipeline);
    const stages = getAggPlanStages(explainRes, getGroupStageName(db));

    assert.eq(stages.length, 2, "Expected 2 group stages in: " + tojson(explainRes));

    // Document distribution across shards based on {_id: 1} shard key:
    // Shard 0 (last-lts): _id 0-5: Coffee(4), Specialty(1), Tea(1) => 3 groups
    // Shard 1 (latest):   _id 6-11: Coffee(1), Specialty(2), Tea(2) => 3 groups
    assert.eq(3, stages[0].nReturned);
    assert.eq(3, stages[1].nReturned);
    const stagesWithMemoryTracking = stages.filter((stage) => stage.hasOwnProperty("peakTrackedMemBytes"));
    const stagesWithoutMemoryTracking = stages.filter((stage) => !stage.hasOwnProperty("peakTrackedMemBytes"));

    assert.eq(stagesWithMemoryTracking.length, 1, "Expected exactly 1 stage with memory tracking: " + tojson(stages));
    assert.eq(
        stagesWithoutMemoryTracking.length,
        1,
        "Expected exactly 1 stage without memory tracking: " + tojson(stages),
    );
    assert.gt(
        stagesWithMemoryTracking[0].peakTrackedMemBytes,
        0,
        "Expected positive peakTrackedMemBytes: " + tojson(stagesWithMemoryTracking[0]),
    );

    // For a mixed version cluster, $currentOp should reflect the mongos binary version.
    checkCurrentOpMemoryTracking("group", primaryConn, db, db[collName], pipeline, false /*expectMemoryMetrics*/);
}

function assertMemoryTrackingMetricsAppearInExplainButNotCurOp(primaryConn) {
    const db = getDB(primaryConn);
    setServerParams(db);

    jsTest.log.info("Checking that logs do not have memory tracking stats...");
    assertMemoryTrackingSlowQueryLogMetrics(db, false /*expectMemoryMetrics*/);

    jsTest.log.info("Checking that memory tracking stats appear in explain...");
    assertMemoryTrackingExplainExecutionStatsMetrics(db, true /*expectMemoryMetrics*/);

    // For a mixed version cluster, $currentOp should reflect the mongos binary version.
    checkCurrentOpMemoryTracking("group", primaryConn, db, db[collName], pipeline, false /*expectMemoryMetrics*/);
}

/**
 * featureFlagQueryMemoryTracking is not FCV-gated, so memory tracking is available as soon as we
 * upgrade the binaries.
 */
testPerformUpgradeReplSet({
    setupFn: setupCollection,
    whenFullyDowngraded: assertMemoryTrackingMetricsDoNotAppear,
    whenSecondariesAreLatestBinary: assertMemoryTrackingMetricsDoNotAppear,
    whenBinariesAreLatestAndFCVIsLastLTS: assertMemoryTrackingMetricsAppear,
    whenFullyUpgraded: assertMemoryTrackingMetricsAppear,
});

/**
 * Some explanation of the trickier mixed-version cluster behaviors:
 *
 * whenSecondariesAndConfigAreLatestBinary:
 * The second shard is upgraded to latest but the first shard is still on last-lts.
 *
 * We expect to see memory tracking metrics to partially appear for explain("executionStats"). Each
 * shard will perform a group-- the group that runs on the second shard will have memory metrics,
 * but not the group that runs on the first shard.
 *
 * Because mongos is still last-lts, we do not expect to see any memory tracking metrics in channels
 * that pull stats from CurOp (slow query logs and $currentOp).
 *
 * whenMongosBinaryIsLastLTS:
 * mongos can be last-lts but the shards themselves are upgraded to the latest version.
 *
 * In this situation, we expect to see memory tracking metrics fully in explain since both
 * shards are executing a memory-tracked stage. We don't expect to see any CurOp memory metrics
 * because we are asking mongos--who is on a version that does not have memory-tracking enabled--for
 * its slow query logs/$currentOp output.
 *
 * whenBinariesAreLatestAndFCVIsLastLTS:
 * All binaries are latest, but the FCV is last-lts.
 *
 * In this situation, we expect to see memory tracking metrics fully in all channels because the
 * feature is not FCV-gated.
 */
testPerformUpgradeSharded({
    setupFn: setupCollection,
    whenFullyDowngraded: assertMemoryTrackingMetricsDoNotAppear,
    whenOnlyConfigIsLatestBinary: assertMemoryTrackingMetricsDoNotAppear,
    whenSecondariesAndConfigAreLatestBinary: assertMemoryTrackingMetricsAppearOnOneShard,
    whenMongosBinaryIsLastLTS: assertMemoryTrackingMetricsAppearInExplainButNotCurOp,
    whenBinariesAreLatestAndFCVIsLastLTS: assertMemoryTrackingMetricsAppear,
    whenFullyUpgraded: assertMemoryTrackingMetricsAppear,
});
