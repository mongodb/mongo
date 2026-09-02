/**
 * Tests that 'primaryDrivenIndexBuildScanPhaseContainerWriteMBperSec' throttles the rate at
 * which the collection scan phase of a primary-driven index build spills sorted keys into the
 * sorter's container.
 *
 * The build is set up to spill (a small 'maxIndexBuildMemoryUsageMegabytes' against a collection
 * whose keys are much larger than that), then run under a byte rate low enough that it cannot
 * possibly finish quickly. The test asserts the build is still in progress well past the point
 * where it would otherwise have completed, then clears the knob at runtime and asserts the build
 * finishes and produces a usable index.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const dbName = "test";
const collName = jsTestName();
const indexName = "x_1";

// The minimum the server accepts, so that the scan starts spilling as early as possible.
const kMaxIndexBuildMemoryUsageMegabytes = 50;
// Individual container writes are this large, and the throttle is charged between them, so this
// also bounds how far past the limit a single write can push the throttle, and therefore how long
// one wait can be.
const kSorterInsertionBatchBytes = 256 * 1024;
// 1 MB/s against the ~100 MB the collection below spills, so a throttled scan needs roughly 100
// seconds and cannot complete within the observation window.
const kThrottledMBperSec = 1;
// How long to let the throttled build run before concluding that it is in fact being held back.
const kObservationWindowMillis = 10 * 1000;

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(dbName);

// TODO(SERVER-109578): Remove these checks when the feature flags are removed.
if (!FeatureFlagUtil.isPresentAndEnabled(db, "PrimaryDrivenIndexBuilds")) {
    jsTest.log.info("Skipping test because featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}
if (!FeatureFlagUtil.isPresentAndEnabled(db, "ContainerWrites")) {
    jsTest.log.info("Skipping test because featureFlagContainerWrites is disabled");
    rst.stopSet();
    quit();
}

const coll = db.getCollection(collName);
const kDocCount = 1200;
const padding = "a".repeat(100 * 1024);
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < kDocCount; i++) {
    bulk.insert({_id: i, x: `${i}-${padding}`});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        maxIndexBuildMemoryUsageMegabytes: kMaxIndexBuildMemoryUsageMegabytes,
        primaryDrivenIndexBuildSorterInsertionBatchBytes: kSorterInsertionBatchBytes,
        primaryDrivenIndexBuildScanPhaseContainerWriteMBperSec: kThrottledMBperSec,
    }),
);

const awaitBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {x: 1});
const opId = IndexBuildTest.waitForIndexBuildToStart(db, collName, indexName);

sleep(kObservationWindowMillis);

// The throttle only paces the scan; it must not have stalled it outright, and it certainly must
// not have let it finish.
IndexBuildTest.assertIndexBuildCurrentOpContents(db, opId, (op) => {
    assert.gt(op.progress.done, 0, "throttled index build made no progress at all", {op});
    assert.lt(
        op.progress.done,
        kDocCount,
        "throttled index build finished its scan despite the byte rate limit",
        {op},
    );
});

// Lifting the throttle at runtime must let the in-progress build proceed. The knob is re-read on
// every charge, i.e. once per container write, so a build already waiting picks the change up as
// soon as its current wait ends rather than having to sit out a batch-sized debt.
assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        primaryDrivenIndexBuildScanPhaseContainerWriteMBperSec: 0,
    }),
);

awaitBuild();
IndexBuildTest.assertIndexes(coll, 2, ["_id_", indexName]);

// The index built under the throttle is complete and correct.
assert.eq(
    1,
    coll
        .find({x: `7-${padding}`})
        .hint(indexName)
        .itcount(),
);
assert.eq(kDocCount, coll.find({}).hint(indexName).itcount());

rst.stopSet();
