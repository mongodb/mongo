/**
 * Validates index build activity, completion, and failure metrics.
 *
 * @tags: [requires_otel_build, requires_replication]
 */

import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {otelFileExportParams} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";
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
            setParameter: {...otelParams},
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

const baselineStart = new Date();
waitForIndexStatusMetrics(
    metricsDir,
    baselineStart,
    (metrics) => metrics.active === 0 && metrics.started === 0 && metrics.succeeded === 0 && metrics.failed === 0,
    "Expected index build metrics to start at zero",
);

jsTest.log.info("Running successful index build");
IndexBuildTest.pauseIndexBuilds(primary);
const successStart = new Date();
const awaitSuccess = IndexBuildTest.startIndexBuild(primary, successColl.getFullName(), {a: 1});

IndexBuildTest.waitForIndexBuildToScanCollection(testDB, successCollName, "a_1");
waitForIndexStatusMetrics(
    metricsDir,
    successStart,
    (metrics) => metrics.active === 1 && metrics.started === 1 && metrics.succeeded === 0 && metrics.failed === 0,
    "Expected active gauge to reflect successful in-progress build",
);

IndexBuildTest.resumeIndexBuilds(primary);
awaitSuccess();

waitForIndexStatusMetrics(
    metricsDir,
    successStart,
    (metrics) => metrics.active === 0 && metrics.started === 1 && metrics.succeeded === 1 && metrics.failed === 0,
    "Expected completed counter to increment after successful build",
);

jsTest.log.info("Running failing unique index build");
IndexBuildTest.pauseIndexBuilds(primary);
const failureStart = new Date();
const awaitFailure = IndexBuildTest.startIndexBuild(primary, failureColl.getFullName(), {a: 1}, {unique: true}, [
    ErrorCodes.DuplicateKey,
]);

IndexBuildTest.waitForIndexBuildToScanCollection(testDB, failureCollName, "a_1");
waitForIndexStatusMetrics(
    metricsDir,
    failureStart,
    (metrics) => metrics.active === 1 && metrics.started === 2 && metrics.succeeded === 1 && metrics.failed === 0,
    "Expected active gauge to reflect failing in-progress build",
);

IndexBuildTest.resumeIndexBuilds(primary);
awaitFailure();

waitForIndexStatusMetrics(
    metricsDir,
    failureStart,
    (metrics) => metrics.active === 0 && metrics.started === 2 && metrics.succeeded === 1 && metrics.failed === 1,
    "Expected failed counter to increment after duplicate-key build failure",
);

jsTest.log.info("Running externally aborted index build");
IndexBuildTest.pauseIndexBuilds(primary);
const abortedStart = new Date();
const awaitAborted = IndexBuildTest.startIndexBuild(primary, abortedColl.getFullName(), {b: 1}, {}, [
    ErrorCodes.IndexBuildAborted,
]);

IndexBuildTest.waitForIndexBuildToScanCollection(testDB, abortedCollName, "b_1");
waitForIndexStatusMetrics(
    metricsDir,
    abortedStart,
    (metrics) => metrics.active === 1 && metrics.started === 3 && metrics.succeeded === 1 && metrics.failed === 1,
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

waitForIndexStatusMetrics(
    metricsDir,
    abortedStart,
    (metrics) => metrics.active === 0 && metrics.started === 3 && metrics.succeeded === 1 && metrics.failed === 2,
    "Expected externally aborted build to increment failed counter",
);

rst.stopSet();
