/**
 * Tests that a primary-driven index build (PDIB) succeeds when WriteConflictExceptions are injected
 * during the spill phase of the container-based sorter (ContainerBasedSpiller::_spill).
 *
 * Drives the spiller by capping maxIndexBuildMemoryUsageMegabytes to its minimum (50MB) and inserting
 * a corpus large enough to force at least one spill. The WTWriteConflictException failpoint is set
 * with a bounded `times` so the writeConflictRetry loop must absorb the conflicts and the build must
 * still complete successfully.
 *
 * Regression coverage for SERVER-118892 (writeConflictRetry around batched inserts in
 * ContainerBasedSpiller::_spill()).
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

// Lower-memory diagnostics path must be off so we hit the production 50MB floor.
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

// Force the sorter to spill aggressively by clamping the memory budget.
assert.commandWorked(primary.adminCommand({setParameter: 1, maxIndexBuildMemoryUsageMegabytes: 50}));

// Insert enough data so that scanning the collection during the index build forces the
// ContainerBasedSpiller through at least one _spill() invocation.
const docCount = 25000;
const padding = "p".repeat(4096); // ~4KB / doc -> ~100MB on-disk, well past the 50MB spill threshold.
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

// Inject a bounded number of WriteConflictExceptions. writeConflictRetry inside
// ContainerBasedSpiller::_spill() must absorb every one of these and the build must succeed.
const writeConflicts = 15;
const fp = configureFailPoint(primary, "WTWriteConflictException", {}, {times: writeConflicts});

assert.commandWorked(coll.createIndex(indexSpec, {name: indexName}));

fp.off();

// The index must be present and usable on the primary.
IndexBuildTest.assertIndexes(coll, /*numIndexes=*/ 2, [indexName], [], {includeBuildUUIDs: false});

// And it must be present on the secondary after replication.
rst.awaitReplication();
const secondary = rst.getSecondary();
IndexBuildTest.assertIndexes(
    secondary.getDB(dbName).getCollection(collName),
    /*numIndexes=*/ 2,
    [indexName],
    [],
    {includeBuildUUIDs: false},
);

rst.stopSet();
