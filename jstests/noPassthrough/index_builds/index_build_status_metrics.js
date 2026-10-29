/**
 * Validates index build activity, completion, duration, and failure metrics, as reported by both
 * OTel and serverStatus.
 *
 * @tags: [requires_otel_build, requires_replication]
 */

import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {otelFileExportParams} from "jstests/noPassthrough/observability/libs/otel_metrics_file_export_helpers.js";
import {waitForIndexStatusMetrics} from "jstests/noPassthrough/index_builds/libs/index_build_otel_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = jsTestName();
const successCollName = "success";
const failureCollName = "failure";
const abortedCollName = "aborted";

const {metricsDir, otelParams} = otelFileExportParams(jsTestName());
const rst = new ReplSetTest({
    nodes: [
        {
            setParameter: {
                ...otelParams,
                openTelemetryExportIntervalMillis: 500,
                openTelemetryExportTimeoutMillis: 200,
            },
        },
    ],
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB(dbName);
const successColl = testDB.getCollection(successCollName);
const failureColl = testDB.getCollection(failureCollName);
const abortedColl = testDB.getCollection(abortedCollName);

assert.commandWorked(
    successColl.insertMany([
        {_id: 1, a: 1},
        {_id: 2, a: 2},
        {_id: 3, a: 3},
    ]),
);
assert.commandWorked(
    failureColl.insertMany([
        {_id: 1, a: 1},
        {_id: 2, a: 1},
        {_id: 3, a: 2},
    ]),
);
assert.commandWorked(
    abortedColl.insertMany([
        {_id: 1, a: 1},
        {_id: 2, a: 2},
        {_id: 3, a: 3},
    ]),
);

const statusFields = [
    "active",
    "started",
    "succeeded",
    "failed",
    "toBeResumed",
    "lastCommittedMillis",
];

// Waits until OTel reports the expected index build status metrics, then checks that serverStatus
// reports the same.
function assertIndexBuildStatusMetrics(afterDate, expectedMetrics, message) {
    waitForIndexStatusMetrics(
        metricsDir,
        afterDate,
        (metrics) => statusFields.every((field) => metrics[field] === expectedMetrics[field]),
        message,
    );

    const indexBuilds = assert.commandWorked(primary.adminCommand({serverStatus: 1})).metrics
        .indexBuilds;

    // lastCommittedMillis is only reported once it is non-zero.
    const serverStatusMetrics = {
        active: Number(indexBuilds.active),
        started: Number(indexBuilds.started),
        succeeded: Number(indexBuilds.succeeded),
        failed: Number(indexBuilds.failed),
        toBeResumed: Number(indexBuilds.toBeResumed),
        lastCommittedMillis: Number(indexBuilds.phases?.lastCommittedMillis ?? 0),
    };
    assert.docEq(expectedMetrics, serverStatusMetrics, "serverStatus metrics do not match");
}

assertIndexBuildStatusMetrics(
    new Date(),
    {active: 0, started: 0, succeeded: 0, failed: 0, toBeResumed: 0, lastCommittedMillis: 0},
    "Expected index build metrics to start at zero",
);

jsTest.log.info("Running successful index build");
IndexBuildTest.pauseIndexBuilds(primary);
const successStart = new Date();
const awaitSuccess = IndexBuildTest.startIndexBuild(primary, successColl.getFullName(), {a: 1});

IndexBuildTest.waitForIndexBuildToScanCollection(testDB, successCollName, "a_1");
assertIndexBuildStatusMetrics(
    successStart,
    {active: 1, started: 1, succeeded: 0, failed: 0, toBeResumed: 0, lastCommittedMillis: 0},
    "Expected active gauge to reflect successful in-progress build",
);

// Pause the build so its committed duration is at least a second.
sleep(1000);
IndexBuildTest.resumeIndexBuilds(primary);
awaitSuccess();

const {lastCommittedMillis} = waitForIndexStatusMetrics(
    metricsDir,
    successStart,
    (metrics) => metrics.lastCommittedMillis >= 1000,
    "Expected last committed duration to be at least a second after successful build",
);
assertIndexBuildStatusMetrics(
    successStart,
    {active: 0, started: 1, succeeded: 1, failed: 0, toBeResumed: 0, lastCommittedMillis},
    "Expected completed counter to update after successful build",
);

jsTest.log.info("Running failing unique index build");
IndexBuildTest.pauseIndexBuilds(primary);
const failureStart = new Date();
const awaitFailure = IndexBuildTest.startIndexBuild(
    primary,
    failureColl.getFullName(),
    {a: 1},
    {unique: true},
    [ErrorCodes.DuplicateKey],
);

IndexBuildTest.waitForIndexBuildToScanCollection(testDB, failureCollName, "a_1");
assertIndexBuildStatusMetrics(
    failureStart,
    {active: 1, started: 2, succeeded: 1, failed: 0, toBeResumed: 0, lastCommittedMillis},
    "Expected active gauge to reflect failing in-progress build",
);

IndexBuildTest.resumeIndexBuilds(primary);
awaitFailure();

assertIndexBuildStatusMetrics(
    failureStart,
    {active: 0, started: 2, succeeded: 1, failed: 1, toBeResumed: 0, lastCommittedMillis},
    "Expected failed counter to increment after duplicate-key build failure, without changing the " +
        "last committed duration",
);

jsTest.log.info("Running externally aborted index build");
IndexBuildTest.pauseIndexBuilds(primary);
const abortedStart = new Date();
const awaitAborted = IndexBuildTest.startIndexBuild(
    primary,
    abortedColl.getFullName(),
    {b: 1},
    {},
    [ErrorCodes.IndexBuildAborted],
);

IndexBuildTest.waitForIndexBuildToScanCollection(testDB, abortedCollName, "b_1");
assertIndexBuildStatusMetrics(
    abortedStart,
    {active: 1, started: 3, succeeded: 1, failed: 1, toBeResumed: 0, lastCommittedMillis},
    "Expected active gauge to reflect aborted in-progress build",
);

TestData.dbName = dbName;
TestData.abortedCollName = abortedCollName;
const awaitDrop = startParallelShell(() => {
    const testDB = db.getSiblingDB(TestData.dbName);
    assert.commandWorked(testDB.runCommand({dropIndexes: TestData.abortedCollName, index: "b_1"}));
}, primary.port);

checkLog.contains(primary, "About to abort index builder");
IndexBuildTest.resumeIndexBuilds(primary);
awaitAborted();
awaitDrop();

assertIndexBuildStatusMetrics(
    abortedStart,
    {active: 0, started: 3, succeeded: 1, failed: 2, toBeResumed: 0, lastCommittedMillis},
    "Expected externally aborted build to increment failed counter",
);

rst.stopSet();
