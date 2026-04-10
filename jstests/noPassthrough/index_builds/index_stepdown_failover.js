/**
 * Confirms that index builds on a stepped down primary are not aborted and will
 * wait for a commitIndexBuild from the new primary before committing.
 * @tags: [
 *   # Though secondaries will wait, in progress primary driven index builds
 *   # are aborted on step up by the new primary.
 *   primary_driven_index_builds_incompatible_due_to_abort_on_step_up,
 *   requires_otel_build,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {waitForIndexStatusMetrics} from "jstests/noPassthrough/index_builds/libs/index_build_otel_utils.js";
import {otelFileExportParams} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

const {metricsDir: primaryMetricsDir, otelParams: primaryOtelParams} = otelFileExportParams(`${jsTestName()}_node0`);
const {metricsDir: secondaryMetricsDir, otelParams: secondaryOtelParams} = otelFileExportParams(
    `${jsTestName()}_node1`,
);
const rst = new ReplSetTest({
    // We want at least two electable nodes.
    nodes: [
        {
            setParameter: {...primaryOtelParams},
        },
        {
            setParameter: {...secondaryOtelParams},
        },
        {arbiter: true},
    ],
});
const nodes = rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const primary = rst.getPrimary();
const primaryNodeId = rst.getNodeId(primary);
const testDB = primary.getDB("test");
const coll = testDB.getCollection("test");
const secondary = rst.getSecondary();
const secondaryNodeId = rst.getNodeId(secondary);
const secondaryDB = secondary.getDB(testDB.getName());
const secondaryColl = secondaryDB.getCollection(coll.getName());

const metricsDirsByNodeId = {
    [primaryNodeId]: primaryMetricsDir,
    [secondaryNodeId]: secondaryMetricsDir,
};

assert.commandWorked(coll.insert({a: 1}));

const baselineStart = new Date();
const primaryBaselineMetrics = waitForIndexStatusMetrics(
    metricsDirsByNodeId[primaryNodeId],
    baselineStart,
    (metrics) => metrics.active === 0,
    "Expected no in-progress index builds on the original primary before failover scenario starts",
);
const secondaryBaselineMetrics = waitForIndexStatusMetrics(
    metricsDirsByNodeId[secondaryNodeId],
    baselineStart,
    (metrics) => metrics.active === 0,
    "Expected no in-progress index builds on the original secondary before failover scenario starts",
);

// Start index build on primary, but prevent it from finishing.
IndexBuildTest.pauseIndexBuilds(primary);
const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

// Wait for the index build to start on the secondary.
IndexBuildTest.waitForIndexBuildToStart(secondaryDB);
const indexMap = IndexBuildTest.assertIndexesSoon(secondaryColl, 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true});
const indexBuildUUID = indexMap["a_1"].buildUUID;

// Index build should be present in the config.system.indexBuilds collection.
assert(primary.getCollection("config.system.indexBuilds").findOne({_id: indexBuildUUID}));

const newPrimary = rst.getSecondary();
const newPrimaryDB = secondaryDB;
const newPrimaryColl = secondaryColl;

// Step down the primary.
// Expect failed createIndex command invocation in parallel shell due to stepdown even though
// the index build will continue in the background.
assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));
const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, "expected shell to exit abnormally due to index build being terminated");
checkLog.containsJson(primary, 20444);

// Unblock the index build on the old primary during the collection scanning phase.
// This index build will not complete because it has to wait for a commitIndexBuild oplog
// entry.
IndexBuildTest.resumeIndexBuilds(primary);
checkLog.containsJson(primary, 3856203);

// Step up the new primary.
rst.stepUp(newPrimary);

// A new index should be present on the old primary after processing the commitIndexBuild oplog
// entry from the new primary.
IndexBuildTest.waitForIndexBuildToStop(testDB);
IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

// Check that index was created on the new primary.
IndexBuildTest.waitForIndexBuildToStop(newPrimaryDB);
IndexBuildTest.assertIndexes(newPrimaryColl, 2, ["_id_", "a_1"]);

// Index build should be removed from the config.system.indexBuilds collection.
assert.isnull(newPrimary.getCollection("config.system.indexBuilds").findOne({_id: indexBuildUUID}));

waitForIndexStatusMetrics(
    metricsDirsByNodeId[primaryNodeId],
    baselineStart,
    (metrics) =>
        metrics.active === primaryBaselineMetrics.active &&
        metrics.started === primaryBaselineMetrics.started + 1 &&
        metrics.succeeded === primaryBaselineMetrics.succeeded + 1 &&
        metrics.failed === primaryBaselineMetrics.failed,
    "Expected the stepped-down primary to record the index build as completed, not failed",
);
waitForIndexStatusMetrics(
    metricsDirsByNodeId[secondaryNodeId],
    baselineStart,
    (metrics) =>
        metrics.active === secondaryBaselineMetrics.active &&
        metrics.started === secondaryBaselineMetrics.started + 1 &&
        metrics.succeeded === secondaryBaselineMetrics.succeeded + 1 &&
        metrics.failed === secondaryBaselineMetrics.failed,
    "Expected the stepped-up secondary to record the index build as completed, not failed",
);

rst.stopSet();
