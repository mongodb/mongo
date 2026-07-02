/**
 * Tests aborting a primary-driven index build due to user write blocking in the scan, load, and
 * drain phases.
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

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = jsTestName();
const primaryDB = primary.getDB(dbName);

const requiredFlags = ["PrimaryDrivenIndexBuilds", "ContainerWrites"];
for (const flag of requiredFlags) {
    if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, flag)) {
        jsTest.log.info("Skipping: featureFlag" + flag + " is disabled");
        rst.stopSet();
        quit();
    }
}

// Force spilling and make both the collection scan and the bulk load yield frequently. A large
// yield period disables time-based scan yields, so the scan yields deterministically after
// `internalQueryExecYieldIterations` documents (well after spilling begins).
assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        maxIndexBuildMemoryUsageMegabytes: 1,
        internalIndexBuildBulkLoadYieldIterations: 1,
        internalQueryExecYieldIterations: 20,
        internalQueryExecYieldPeriodMS: 60000,
    }),
);

// The 64 KB `a` values make the {a: 1} sorter spill quickly; {arr: 1} is multikey.
const indexSpecs = [{a: 1}, {arr: 1}];

function seedSpillingData(coll) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 60; i++) {
        bulk.insert({_id: i, a: "x".repeat(64 * 1024), arr: [i, i + 1, i + 2, i + 3, i + 4]});
    }
    assert.commandWorked(bulk.execute());
}

// Aborts the build (paused at `phaseFp` with its sorter cursors open) via user write blocking, and
// asserts the server survives and the build was aborted. Shared by all three phases.
function abortViaWriteBlockAndAssert(coll, phaseFp, awaitIndexBuild) {
    // Pause the aborter after it has interrupted the build but before it tears the builder down.
    const abortFp = configureFailPoint(primary, "hangBeforeCompletingAbort");
    const awaitWriteBlock = startParallelShell(function () {
        // TODO (SERVER-130409): Remove the retry once setUserWriteBlockMode can no longer fail with
        // WriteConflict.
        retryOnRetryableError(
            () =>
                assert.commandWorked(
                    db.getSiblingDB("admin").runCommand({setUserWriteBlockMode: 1, global: true}),
                ),
            /*numRetries=*/ 100,
            /*sleepMs=*/ 1,
            [ErrorCodes.WriteConflict],
        );
    }, primary.port);

    abortFp.wait();
    phaseFp.off();

    awaitIndexBuild();

    abortFp.off();
    awaitWriteBlock();

    assert.commandWorked(primary.adminCommand({ping: 1}));
    IndexBuildTest.assertIndexesSoon(coll, 1, ["_id_"]);
    // TODO (SERVER-130409): Remove the retry once setUserWriteBlockMode can no longer fail with
    // WriteConflict.
    retryOnRetryableError(
        () => assert.commandWorked(primary.adminCommand({setUserWriteBlockMode: 1, global: false})),
        /*numRetries=*/ 100,
        /*sleepMs=*/ 1,
        [ErrorCodes.WriteConflict],
    );
}

function testCollectionScanPhase() {
    jsTest.log.info("Aborting a spilled build during the collection scan phase");
    const coll = primaryDB.getCollection("scan");
    seedSpillingData(coll);

    // Hang at a collection-scan yield, which happens after ~20 documents -- by which point the
    // {a: 1} sorter has spilled and holds cursors.
    const scanFp = configureFailPoint(primary, "setYieldAllLocksHang", {
        namespace: coll.getFullName(),
    });
    const awaitIndexBuild = IndexBuildTest.startIndexBuild(
        primary,
        coll.getFullName(),
        indexSpecs,
        {},
        [ErrorCodes.IndexBuildAborted],
    );
    scanFp.wait();

    abortViaWriteBlockAndAssert(coll, scanFp, awaitIndexBuild);
}

function testBulkLoadPhase() {
    jsTest.log.info("Aborting a spilled build during the bulk load phase");
    const coll = primaryDB.getCollection("load");
    seedSpillingData(coll);

    const loadFp = configureFailPoint(primary, "hangDuringIndexBuildBulkLoadYield", {
        namespace: coll.getFullName(),
    });
    const awaitIndexBuild = IndexBuildTest.startIndexBuild(
        primary,
        coll.getFullName(),
        indexSpecs,
        {},
        [ErrorCodes.IndexBuildAborted],
    );
    loadFp.wait();

    abortViaWriteBlockAndAssert(coll, loadFp, awaitIndexBuild);
}

function testDrainWritesPhase() {
    jsTest.log.info("Aborting a spilled build during the drain writes phase");
    const coll = primaryDB.getCollection("drain");
    seedSpillingData(coll);

    // Pause first in the load phase so we can generate side writes for the drain to process.
    const loadFp = configureFailPoint(primary, "hangDuringIndexBuildBulkLoadYield", {
        namespace: coll.getFullName(),
    });
    const drainFp = configureFailPoint(primary, "hangDuringIndexBuildDrainYield", {
        namespace: coll.getFullName(),
    });
    const awaitIndexBuild = IndexBuildTest.startIndexBuild(
        primary,
        coll.getFullName(),
        indexSpecs,
        {},
        [ErrorCodes.IndexBuildAborted],
    );
    loadFp.wait();

    // Writes received while the build is in progress become side writes, giving the drain phase work
    // to do (and a yield at which to hang).
    assert.commandWorked(
        coll.insert(Array.from({length: 10}, (_, i) => ({_id: 1000 + i, a: "y", arr: [i]}))),
    );

    // Let the build finish the load and enter the drain phase, where it hangs at a yield.
    loadFp.off();
    drainFp.wait();

    abortViaWriteBlockAndAssert(coll, drainFp, awaitIndexBuild);
}

testCollectionScanPhase();
testBulkLoadPhase();
testDrainWritesPhase();

rst.stopSet();
