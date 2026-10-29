/**
 * Tests that the write rate of the bulk load phase of a primary-driven index build is throttled
 * correctly.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = jsTestName();
const primaryDB = primary.getDB(dbName);

// TODO(SERVER-109578): Remove these checks when the feature flags are removed.
if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "PrimaryDrivenIndexBuilds")) {
    jsTest.log.info("Skipping test because featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}
if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "ContainerWrites")) {
    jsTest.log.info("Skipping test because featureFlagContainerWrites is disabled");
    rst.stopSet();
    quit();
}

const kThrottledMBperSec = 1;

// The limit is charged when the build yields, and the bulk load yields every
// 'internalIndexBuildBulkLoadYieldIterations' keys, which defaults to 1000. Insert a multiple of this value so that we throttle a few times.
const kDocCount = 5000;
// ~4 KB per key, so the load phase writes ~20 MB of index keys in total.
const kPaddingBytes = 4 * 1024;
const padding = "a".repeat(kPaddingBytes);
const kTotalKeyMB = (kDocCount * kPaddingBytes) / (1024 * 1024);

// The upper bound for the throttled index build rate.
const kExpectedThrottledMillis = (kTotalKeyMB / kThrottledMBperSec) * 1000; // ~20 seconds.
// Watch the throttled build for a fraction of the upper bound of the duration, erring on the lower end which should still be greater than the unthrottled index build duration but gives some wiggle room for the index build completing faster than expected.
const kSleepDuration = kExpectedThrottledMillis / 4;

const coll = primaryDB.getCollection("coll");

function setRate(mbPerSec) {
    assert.commandWorked(
        primary.adminCommand({
            setParameter: 1,
            primaryDrivenIndexBuildBulkLoadPhaseContainerWriteMBperSec: mbPerSec,
        }),
    );
}

/**
 * Returns the progress meter of the operation loading keys into the index, or null if no build is in
 * that phase.
 */
function bulkLoadProgress() {
    const ops = primaryDB
        .getSiblingDB("admin")
        .aggregate([
            {$currentOp: {allUsers: true, idleConnections: true}},
            {$match: {ns: coll.getFullName(), "progress.total": {$exists: true}}},
        ])
        .toArray();
    const loading = ops.filter((op) => op.msg && op.msg.includes("external sorter into index"));
    assert.lte(loading.length, 1, "expected at most one index build loading keys", {ops});
    return loading.length === 1 ? loading[0].progress : null;
}

describe("the bulk load phase write rate limit", function () {
    before(function () {
        const bulk = coll.initializeUnorderedBulkOp();
        // Create two fields that have the same values, 'a' and 'b', to be used when comparing the unthrottled and throttled index builds.
        for (let i = 0; i < kDocCount; i++) {
            const value = `${i} - ${padding}`;
            bulk.insert({_id: i, a: value, b: value});
        }
        assert.commandWorked(bulk.execute());
    });

    after(function () {
        setRate(0);
        rst.stopSet();
    });

    it("throttled and unthrottled index builds", function () {
        // Time an unthrottled build on the field 'a'.
        setRate(0);
        const controlStart = Date.now();
        assert.commandWorked(coll.createIndex({a: 1}));
        const unthrottledMillis = Date.now() - controlStart;
        jsTest.log.info("Unthrottled build finished", {unthrottledMillis});
        assert.lt(
            unthrottledMillis,
            kSleepDuration,
            "unthrottled build took longer than the duration that we will watch the unthrottled index build for",
            {unthrottledMillis, kSleepDuration},
        );

        // Time a throttled build on the field 'b'.
        setRate(kThrottledMBperSec);
        const awaitIndexBuild = IndexBuildTest.startIndexBuild(
            primary,
            coll.getFullName(),
            {b: 1},
            {name: "b_1"},
        );
        const opId = IndexBuildTest.waitForIndexBuildToStart(primaryDB, coll.getName(), "b_1");

        // Wait for the build to reach the phase under test before watching it.
        let progress;
        assert.soon(
            () => (progress = bulkLoadProgress()) !== null,
            "index build never reached the bulk load phase",
        );
        const keysLoadedBefore = progress.done;
        // Sleep and let the throttled build run for a while.
        sleep(kSleepDuration);

        // Still loading: the limit should throttle the index build but not stall it. The build should not have completed yet.
        progress = bulkLoadProgress();
        assert.neq(progress, null, "throttled index build left the bulk load phase too early", {
            unthrottledMillis,
            kSleepDuration,
            kExpectedThrottledMillis,
        });
        assert.gt(
            progress.done,
            keysLoadedBefore,
            "throttled index build made no progress at all",
            {
                progress,
                keysLoadedBefore,
            },
        );
        assert.lt(
            progress.done,
            progress.total,
            "throttled index build finished its load despite the rate limit",
            {progress, unthrottledMillis, kSleepDuration, kExpectedThrottledMillis},
        );
        IndexBuildTest.assertIndexBuildCurrentOpContents(primaryDB, opId, (op) => {
            assert.eq(op.ns, coll.getFullName(), "unexpected operation for this index build", {op});
        });

        // Clearing the limit must let the in-progress build proceed: the rate is re-read after
        // every yield, so the new rate would take effect the next time the build yields.
        setRate(0);
        awaitIndexBuild();

        IndexBuildTest.assertIndexes(coll, 3, ["_id_", "a_1", "b_1"]);

        // The index built under the limit is complete and correct.
        assert.eq(kDocCount, coll.find({}).hint("b_1").itcount());
    });
});
