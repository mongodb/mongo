/**
 * Tests that the "dropDatabase" command can abort in-progress index builds on all the collections
 * it is dropping.
 *
 * @tags: [requires_otel_build]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {otelFileExportParams} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";
import {waitForIndexStatusMetrics} from "jstests/noPassthrough/index_builds/libs/index_build_otel_utils.js";

const {metricsDir, otelParams} = otelFileExportParams(jsTestName());
const mongodOptions = {
    setParameter: {...otelParams},
};
const conn = MongoRunner.runMongod(mongodOptions);

const dbName = "drop_database_aborts_in_progress_index_builds";
const firstCollName = "first";
const secondCollName = "second";

TestData.dbName = dbName;

const testDB = conn.getDB(dbName);
testDB.getCollection(firstCollName).drop();
testDB.getCollection(secondCollName).drop();

assert.commandWorked(testDB.createCollection(firstCollName));
assert.commandWorked(testDB.createCollection(secondCollName));
const firstColl = testDB.getCollection(firstCollName);
const secondColl = testDB.getCollection(secondCollName);

assert.commandWorked(firstColl.insert({a: 1}));
assert.commandWorked(firstColl.insert({b: 1}));

assert.commandWorked(secondColl.insert({a: 1}));
assert.commandWorked(secondColl.insert({b: 1}));

assert.commandWorked(firstColl.createIndex({a: 1}));
assert.commandWorked(secondColl.createIndex({a: 1}));

const baselineStart = new Date();
const baselineMetrics = waitForIndexStatusMetrics(
    metricsDir,
    baselineStart,
    (metrics) => metrics.active === 0,
    "Expected no in-progress index builds before dropDatabase scenario starts",
);

jsTest.log("Starting an index build on each collection and freezing them.");
IndexBuildTest.pauseIndexBuilds(testDB.getMongo());

const awaitFirstIndexBuild = IndexBuildTest.startIndexBuild(testDB.getMongo(), firstColl.getFullName(), {b: 1}, {}, [
    ErrorCodes.IndexBuildAborted,
]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, firstCollName, "b_1");

const awaitSecondIndexBuild = IndexBuildTest.startIndexBuild(testDB.getMongo(), secondColl.getFullName(), {b: 1}, {}, [
    ErrorCodes.IndexBuildAborted,
]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, secondCollName, "b_1");

jsTest.log("Dropping database " + dbName + " with in-progress index builds on its collections.");

const failPoint = configureFailPoint(testDB, "dropDatabaseHangAfterWaitingForIndexBuilds");

const awaitDropDatabase = startParallelShell(() => {
    const testDB = db.getSiblingDB(TestData.dbName);
    assert.commandWorked(testDB.dropDatabase());
}, conn.port);
try {
    checkLog.contains(
        testDB.getMongo(),
        "About to abort all index builders running for collections in the given database",
    );

    failPoint.wait();

    // Cannot create a collection on the database while it is drop pending.
    assert.commandFailedWithCode(testDB.createCollection("third"), ErrorCodes.DatabaseDropPending);

    // Performing a bulk write should throw DatabaseDropPending.
    let bulk = testDB["third"].initializeUnorderedBulkOp();
    bulk.insert({});

    try {
        bulk.execute();
    } catch (ex) {
        assert.eq(true, ex instanceof BulkWriteError);
        assert.writeErrorWithCode(ex, ErrorCodes.DatabaseDropPending);
    }
} finally {
    IndexBuildTest.resumeIndexBuilds(testDB.getMongo());
    failPoint.off();
}

awaitFirstIndexBuild();
awaitSecondIndexBuild();
awaitDropDatabase();

waitForIndexStatusMetrics(
    metricsDir,
    baselineStart,
    (metrics) =>
        metrics.active === baselineMetrics.active &&
        metrics.started === baselineMetrics.started + 2 &&
        metrics.succeeded === baselineMetrics.succeeded &&
        metrics.failed === baselineMetrics.failed + 2,
    "Expected dropDatabase to count both aborted index builds as failed",
);

MongoRunner.stopMongod(conn);
