/**
 * Tests a resumable primary-driven index build that is too small to spill and then gets interrupted
 * in the load phase. Upon step up, it is resumed in the scan phase and must clean up the orphaned
 * entries in the index table from the partial pre-resume load.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            primaryDrivenIndexBuildLoadResumeStateWriteIntervalKeys: 10,
            // Commit each loaded key immediately so the first half of the keys are durably in the
            // container (and replicated) before stepdown -- otherwise the half-load sits in an
            // uncommitted batch and is lost, leaving nothing for the resumed load to re-emit.
            primaryDrivenIndexBuildIndexInsertionBatchSize: 1,
        },
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondaries()[0];

const adminDB = primary.getDB("admin");
if (
    !FeatureFlagUtil.isPresentAndEnabled(adminDB, "ContainerWrites") ||
    !FeatureFlagUtil.isPresentAndEnabled(adminDB, "PrimaryDrivenIndexBuilds") ||
    !FeatureFlagUtil.isPresentAndEnabled(adminDB, "ResumablePrimaryDrivenIndexBuilds")
) {
    jsTest.log.info("Skipping test because primary-driven index builds are not enabled");
    rst.stopSet();
    quit();
}

const dbName = "test";
const collName = jsTestName();
const indexName = "a_1";
const primaryColl = primary.getDB(dbName).getCollection(collName);

const numDocs = 100;
const pauseIteration = Math.floor(numDocs / 2); // Load the first half (a=1..50), then hang.

jsTest.log.info("Seeding a=1..100 (small index: never spills)");
{
    const docs = [];
    for (let i = 1; i <= numDocs; i++) {
        docs.push({_id: i, a: i});
    }
    assert.commandWorked(primaryColl.insert(docs));
}
rst.awaitReplication();

// Pause the load phase halfway (after the first half of the keys are written to the index table).
const loadFp = configureFailPoint(primary, "hangIndexBuildDuringBulkLoadPhase", {
    indexNames: [indexName],
    iteration: NumberLong(pauseIteration),
});

jsTest.log.info("Starting the index build (will be interrupted by stepdown)");
const awaitBuild = IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {a: 1}, {}, [
    ErrorCodes.InterruptedDueToReplStateChange,
    ErrorCodes.IndexBuildAborted,
]);

jsTest.log.info("Waiting for the load to reach the pause point (scan done, first half loaded)");
loadFp.wait();

// Concurrent insert: key a=0 sorts BEFORE every loaded key, and was not part of the original scan.
jsTest.log.info("Inserting a=0 (sorts first; not yet loaded)");
assert.commandWorked(
    primary
        .getDB(dbName)
        .getCollection(collName)
        .insert({_id: 0, a: 0}, {writeConcern: {w: "majority"}}),
);
rst.awaitReplication();

jsTest.log.info("Stepping up the secondary; the new primary resumes (and re-scans) the build");
// Step up before releasing the fail point: the state change interrupts the old primary's build
// thread at the pause point, whereas releasing the fail point first would let the build run to
// completion on the old primary before we ever step up.
rst.stepUp(secondary);
try {
    awaitBuild();
} catch (e) {
    jsTest.log.info("index build shell exited (expected after stepdown)", {error: e});
}
loadFp.off();

const newPrimary = rst.getPrimary();

jsTest.log.info("Waiting for the resumed index build to complete on the new primary");
assert.soonNoExcept(
    () =>
        newPrimary
            .getDB(dbName)
            .getCollection(collName)
            .getIndexes()
            .some((i) => i.name === indexName),
    "resumed index build did not complete on the new primary",
);

rst.awaitReplication();
rst.stopSet();
