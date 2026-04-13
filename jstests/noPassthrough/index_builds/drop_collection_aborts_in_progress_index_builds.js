/**
 * Tests that the "drop" command can abort in-progress index builds.
 *
 * @tags: [requires_otel_build]
 */
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {otelFileExportParams} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";
import {waitForIndexStatusMetrics} from "jstests/noPassthrough/index_builds/libs/index_build_otel_utils.js";

const {metricsDir, otelParams} = otelFileExportParams(jsTestName());
const mongodOptions = {
    setParameter: {...otelParams},
};
const conn = MongoRunner.runMongod(mongodOptions);

const dbName = jsTestName();
const collName = "test";

TestData.dbName = dbName;
TestData.collName = collName;

const testDB = conn.getDB(dbName);
testDB.getCollection(collName).drop();

assert.commandWorked(testDB.createCollection(collName));
const coll = testDB.getCollection(collName);

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({b: 1}));

assert.commandWorked(coll.createIndex({a: 1}));

const baselineStart = new Date();
const baselineMetrics = waitForIndexStatusMetrics(
    metricsDir,
    baselineStart,
    (metrics) => metrics.active === 0,
    "Expected no in-progress index builds before drop scenario starts",
);

jsTest.log("Starting two index builds and freezing them.");
IndexBuildTest.pauseIndexBuilds(testDB.getMongo());

const awaitFirstIndexBuild = IndexBuildTest.startIndexBuild(testDB.getMongo(), coll.getFullName(), {a: 1, b: 1}, {}, [
    ErrorCodes.IndexBuildAborted,
]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "a_1_b_1");

const awaitSecondIndexBuild = IndexBuildTest.startIndexBuild(testDB.getMongo(), coll.getFullName(), {b: 1}, {}, [
    ErrorCodes.IndexBuildAborted,
]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "b_1");

jsTest.log("Dropping collection " + dbName + "." + collName + " with in-progress index builds");
const awaitDrop = startParallelShell(() => {
    const testDB = db.getSiblingDB(TestData.dbName);
    assert.commandWorked(testDB.runCommand({drop: TestData.collName}));
}, conn.port);

checkLog.containsJson(testDB.getMongo(), 23879); // "About to abort all index builders"

awaitFirstIndexBuild();
awaitSecondIndexBuild();
awaitDrop();

IndexBuildTest.resumeIndexBuilds(testDB.getMongo());

waitForIndexStatusMetrics(
    metricsDir,
    baselineStart,
    (metrics) =>
        metrics.active === baselineMetrics.active &&
        metrics.started === baselineMetrics.started + 2 &&
        metrics.succeeded === baselineMetrics.succeeded &&
        metrics.failed === baselineMetrics.failed + 2,
    "Expected drop to count both aborted index builds as failed",
);

MongoRunner.stopMongod(conn);
