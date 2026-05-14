/**
 * End-to-end PDIB hardening test: a single index build is exercised across the spill, merge, and
 * drain phases all while WriteConflictExceptions are continuously injected at a moderate rate
 * (`skip` semantics so the failpoint fires intermittently rather than burst-and-done).
 *
 * The combined harness catches regressions where any one of the three writeConflictRetry loops
 * (ContainerBasedSpiller::_spill, ContainerBasedSpiller::mergeSpills insert path,
 * ContainerBasedSpiller::mergeSpills remove path, plus the side_writes_tracker / multi_index_block
 * drain inserts) is removed or weakened — even if the per-phase tests still pass under bursty
 * `times` semantics.
 *
 * Companion full-stack coverage for SERVER-118892 + SERVER-122301.
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

// Aggressive spill threshold so spill + merge happen.
assert.commandWorked(primary.adminCommand({setParameter: 1, maxIndexBuildMemoryUsageMegabytes: 50}));

// Seed a moderately large corpus so the scan + spill + merge phases all do real work.
const seed = 30000;
const padding = "c".repeat(4096);
let d = 0;
const batchSize = 500;
while (d < seed) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < batchSize && d + i < seed; i++) {
        bulk.insert({_id: d + i, x: d + i, pad: padding});
    }
    assert.commandWorked(bulk.execute());
    d += batchSize;
}
rst.awaitReplication();

// Pause the build long enough to stage some side writes (drain coverage).
IndexBuildTest.pauseIndexBuilds(primary);
const awaitIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), indexSpec, {name: indexName});
IndexBuildTest.waitForIndexBuildToStart(db, collName, indexName);

const sideWrites = 1000;
let sideBulk = coll.initializeUnorderedBulkOp();
for (let i = seed; i < seed + sideWrites; i++) {
    sideBulk.insert({_id: i, x: i, pad: padding});
}
assert.commandWorked(sideBulk.execute());

// `skip` semantics: drop the failpoint check on the first 50 calls, then fire on every call until
// the build is finished and `fp.off()` is called. This produces sustained, intermittent WCEs across
// every remaining write — spill, merge, and drain alike — instead of a single burst at one phase.
const fp = configureFailPoint(primary, "WTWriteConflictException", {}, {skip: 50});

IndexBuildTest.resumeIndexBuilds(primary);
awaitIndex();

fp.off();

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

// Full-corpus coverage check via the new index.
const expectedCount = seed + sideWrites;
assert.eq(coll.find({}).hint({x: 1}).itcount(), expectedCount);

rst.stopSet();
