/**
 * Confirms that background index builds on a primary are aborted when the node steps down between
 * scheduling on the thread pool and initialization.
 * @tags: [
 *   requires_otel_build,
 *   requires_replication,
 * ]
 */
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForIndexStatusMetrics} from "jstests/noPassthrough/index_builds/libs/index_build_otel_utils.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {otelFileExportParams} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

const {metricsDir, otelParams} = otelFileExportParams(jsTestName());
const rst = new ReplSetTest({
    nodes: [
        {
            setParameter: {...otelParams},
        },
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ],
});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const coll = testDB.getCollection("test");

assert.commandWorked(coll.insert({a: 1}));

const baselineStart = new Date();
const baselineMetrics = waitForIndexStatusMetrics(
    metricsDir,
    baselineStart,
    (metrics) => metrics.active === 0,
    "Expected no in-progress index builds before the stepdown-before-init scenario starts",
);

const res = assert.commandWorked(
    primary.adminCommand({configureFailPoint: "hangBeforeInitializingIndexBuild", mode: "alwaysOn"}),
);
const failpointTimesEntered = res.count;

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

try {
    assert.commandWorked(
        primary.adminCommand({
            waitForFailPoint: "hangBeforeInitializingIndexBuild",
            timesEntered: failpointTimesEntered + 1,
            maxTimeMS: kDefaultWaitForFailPointTimeout,
        }),
    );

    waitForIndexStatusMetrics(
        metricsDir,
        baselineStart,
        (metrics) =>
            metrics.active === baselineMetrics.active + 1 &&
            metrics.started === baselineMetrics.started + 1 &&
            metrics.succeeded === baselineMetrics.succeeded &&
            metrics.failed === baselineMetrics.failed,
        "Expected the active gauge to reflect the build while it is hung before initialization",
    );

    // Step down the primary.
    assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));
} finally {
    assert.commandWorked(primary.adminCommand({configureFailPoint: "hangBeforeInitializingIndexBuild", mode: "off"}));
}

// Wait for the index build to stop.
IndexBuildTest.waitForIndexBuildToStop(testDB);

const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, "expected shell to exit abnormally due to index build being terminated");

// With both single-phase and two-phase index builds, a stepdown at this point will abort the index
// build because the builder thread cannot generate an optime. Wait for the command thread, not the
// IndexBuildsCoordinator, to report the index build as failed.
checkLog.containsJson(primary, 20449);

waitForIndexStatusMetrics(
    metricsDir,
    baselineStart,
    (metrics) =>
        metrics.active === baselineMetrics.active &&
        metrics.started === baselineMetrics.started + 1 &&
        metrics.succeeded === baselineMetrics.succeeded &&
        metrics.failed === baselineMetrics.failed + 1,
    "Expected stepping down before initialization to count as a failed index build",
);

// Check that no new index has been created.  This verifies that the index build was aborted
// rather than successfully completed.
IndexBuildTest.assertIndexes(coll, 1, ["_id_"]);

const secondaryColl = rst.getSecondary().getCollection(coll.getFullName());
IndexBuildTest.assertIndexes(secondaryColl, 1, ["_id_"]);

rst.stopSet();
