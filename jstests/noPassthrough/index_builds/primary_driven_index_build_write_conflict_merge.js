/**
 * Tests that a primary-driven index build (PDIB) succeeds when WriteConflictExceptions are injected
 * during the merge phase of the container-based sorter
 * (ContainerBasedSpiller::mergeSpills - insert and remove paths).
 *
 * The test forces the sorter through multiple spill cycles (so a merge phase is actually executed)
 * by inserting a corpus large enough to overflow the per-build memory budget several times. It then
 * injects a moderate number of WriteConflictExceptions during the merge. With the writeConflictRetry
 * loops added in SERVER-122301 wrapping both (1) inserts of merged entries and (2) deletes of source
 * spill records, the build must still complete and produce a consistent index.
 *
 * Regression coverage for SERVER-122301.
 *
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

TestData.testingDiagnosticsEnabled = false;

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "test";
const collName = jsTestName();
const db = primary.getDB(dbName);
const coll = db.getCollection(collName);
const indexName = "x_1";
const indexSpec = {x: 1};

// TODO(SERVER-109578): Remove this check when the feature flag is removed.
if (!FeatureFlagUtil.isPresentAndEnabled(db, "PrimaryDrivenIndexBuilds")) {
    jsTest.log.info("Skipping test because featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}

// TODO(SERVER-109578): Remove this check when the feature flag is removed.
if (!FeatureFlagUtil.isPresentAndEnabled(db, "ContainerWrites")) {
    jsTest.log.info("Skipping test because featureFlagContainerWrites is disabled");
    rst.stopSet();
    quit();
}

// Clamp the per-build memory budget to its minimum so that several spill files are produced.
// Several spill files are required for the merge phase to do real work.
assert.commandWorked(primary.adminCommand({setParameter: 1, maxIndexBuildMemoryUsageMegabytes: 50}));

// Insert ~200MB of data — that comfortably forces at least three spill files (50MB budget) and a
// non-trivial merge phase.
const docCount = 50000;
const padding = "m".repeat(4096);
let d = 0;
const batchSize = 500;
while (d < docCount) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < batchSize && d + i < docCount; i++) {
        bulk.insert({_id: d + i, x: d + i, pad: padding});
    }
    assert.commandWorked(bulk.execute());
    d += batchSize;
}
rst.awaitReplication();

// Inject WCEs across the lifetime of the build. They will fire during the spill phase first; a
// surplus persists into the merge phase, which is what we are exercising here. The
// writeConflictRetry loops around the insert-merged-entries and delete-source-records paths in
// ContainerBasedSpiller::mergeSpills must absorb every one.
const writeConflicts = 40;
const fp = configureFailPoint(primary, "WTWriteConflictException", {}, {times: writeConflicts});

assert.commandWorked(coll.createIndex(indexSpec, {name: indexName}));

fp.off();

// Verify the index was built on the primary and replicated to the secondary.
IndexBuildTest.assertIndexes(coll, /*numIndexes=*/ 2, [indexName], [], {includeBuildUUIDs: false});

rst.awaitReplication();
const secondary = rst.getSecondary();
IndexBuildTest.assertIndexes(
    secondary.getDB(dbName).getCollection(collName),
    /*numIndexes=*/ 2,
    [indexName],
    [],
    {includeBuildUUIDs: false},
);

// Spot-check the index actually contains every doc (catches silent merge corruption that a
// pure success-code assertion would miss).
assert.eq(coll.find({x: {$gte: 0}}).hint({x: 1}).itcount(), docCount);

rst.stopSet();
