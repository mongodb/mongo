/**
 * SERVER-126266: stepdown must not deadlock with an in-flight primary-driven
 * index build (PDIB).
 *
 * Bug shape: the stepdown thread enqueues an X waiter on the RSTL. BgSync
 * then parks behind the X waiter (RSTL is fair), so commit-quorum vote
 * oplog entries from secondaries cannot be applied on the primary. The
 * PDIB coordinator, still holding its IX intent, blocks awaiting those
 * votes, which closes the wait-for cycle. Stepdown's tryToStepDown()
 * retry loop never observes the secondaries catching up.
 *
 * This test pauses PDIB in its commit-quorum wait via a failpoint,
 * issues a non-force stepdown, and asserts that stepdown completes
 * within a bounded interval. The matching TLA+ model
 * src/mongo/tla_plus/IndexBuilds/StepdownPDIBDeadlock/ proves the same
 * scenario at the lock-graph level.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 *   requires_fcv_90,
 *   resumable_primary_driven_index_builds_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const dbName = jsTestName();
const collName = "coll";
const indexName = "a_1";
const indexSpec = {a: 1};

// Three-node set: one arbiter so the stepdown target has only one voting
// secondary whose vote must be applied on the (stepping-down) primary
// for the commit quorum to advance. This is the smallest topology that
// surfaces the SERVER-126266 wait-for cycle.
const rst = new ReplSetTest({
    nodes: [
        {},
        {rsConfig: {priority: 0}},
        {arbiter: true},
    ],
});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

let primary = rst.getPrimary();
const secondary = rst.getSecondary();

// Skip on builds without the PDIB feature flag.
if (!FeatureFlagUtil.isPresentAndEnabled(primary.getDB(dbName), "PrimaryDrivenIndexBuilds")) {
    jsTest.log.info("Skipping: featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}

jsTest.log.info("1. Seed data so the index build has work to do.");
assert.commandWorked(primary.getDB(dbName).createCollection(collName));
const bulk = primary.getDB(dbName).getCollection(collName).initializeUnorderedBulkOp();
for (let i = 0; i < 200; i++) {
    bulk.insert({_id: i, a: i});
}
assert.commandWorked(bulk.execute({w: "majority"}));
rst.awaitReplication();

const ns = primary.getDB(dbName).getCollection(collName).getFullName();

jsTest.log.info("2. Pause PDIB on the primary at the commit-quorum signal.");
// hangIndexBuildBeforeSignalPrimaryForCommitReadiness is the canonical
// PDIB pause point: the build coordinator has finished its scan and is
// about to read the votes pushed in by secondaries' OpObserver.
const pdibFp = configureFailPoint(primary, "hangIndexBuildBeforeSignalPrimaryForCommitReadiness");

jsTest.log.info("3. Kick off the index build on the primary.");
const awaitIndexBuild = IndexBuildTest.startIndexBuild(
    primary,
    ns,
    indexSpec,
    {name: indexName},
    [ErrorCodes.InterruptedDueToReplStateChange, ErrorCodes.IndexBuildAborted],
);

jsTest.log.info("4. Wait for PDIB to park at the failpoint.");
pdibFp.wait();

jsTest.log.info("5. Issue a non-force stepdown with a short stepDownSecs.");
// The bug surfaces on non-force stepdown: force=true bypasses the
// secondary-catch-up wait that the deadlock hinges on. A non-force
// stepdown must (a) wait for secondaries to catch up to optime, then
// (b) acquire the RSTL in X. With the fix in place, PDIB observes the
// stepdown signal, releases its RSTL IX intent, BgSync drains the
// vote-bearing oplog entries on secondaries, and stepdown completes.
const stepDownDeadlineMs = 60 * 1000;
const stepDownStart = Date.now();

let stepDownErr = null;
try {
    assert.commandWorked(
        primary.adminCommand({
            replSetStepDown: 30,
            secondaryCatchUpPeriodSecs: 25,
            force: false,
        }),
    );
} catch (e) {
    // The stepDown response is by convention a network error because the
    // RSTL flip closes the user-connection. Either a clean OK or a
    // recognisable HostUnreachable-style error is acceptable; what we
    // are testing for is timely completion, not the response shape.
    stepDownErr = e;
}

const elapsed = Date.now() - stepDownStart;
jsTest.log.info(`6. Stepdown returned after ${elapsed}ms (err=${stepDownErr})`);
assert.lt(
    elapsed,
    stepDownDeadlineMs,
    `Stepdown took >= ${stepDownDeadlineMs}ms; SERVER-126266 deadlock regressed`,
);

jsTest.log.info("7. Release the PDIB failpoint so the build can unwind.");
// With the fix, PDIB has already been aborted via the stepdown
// callback and the build is no longer parked; turning the failpoint off
// is a no-op-or-cleanup. Keep the off() call so the test is robust to
// the build still being on the failpoint at this instant.
pdibFp.off();

jsTest.log.info("8. The former primary is now SECONDARY.");
rst.awaitSecondaryNodes(null, [primary]);

jsTest.log.info("9. Index-build shell exits with an expected failure code.");
awaitIndexBuild({checkExitSuccess: false});

jsTest.log.info("10. A new primary takes over and resumes the workload.");
rst.awaitNodesAgreeOnPrimary();
const newPrimary = rst.getPrimary();
assert.neq(newPrimary.host, primary.host, "Stepdown target is still primary");

// Smoke: write a doc on the new primary to confirm the replica set is
// not wedged.
assert.commandWorked(
    newPrimary.getDB(dbName).getCollection(collName).insert({_id: "post-stepdown", a: 999}, {writeConcern: {w: 1}}),
);

rst.stopSet();
