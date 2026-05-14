/**
 * SERVER-126474: When a primary-driven index build is aborted during step-up, the abort runs in a
 * WriteUnitOfWork that registers the build's storage tables with the drop-pending reaper. If the
 * registration is performed unconditionally inside the WUOW (rather than from an onCommit callback)
 * and the WUOW is rolled back by a concurrent stepdown, the ident remains on the reaper list even
 * though the abort never committed. A subsequent step-up retries the abort with the same buildUUID
 * and indexBuildIdent, which crashes the reaper's "ident already present" invariant.
 *
 * This test exercises that interleaving: drive a PDIB step-up abort, force the abort's WUOW to
 * roll back via a forced stepdown, then step the same node up again so it re-enters the abort
 * path. The second abort must succeed without invariant.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   resumable_primary_driven_index_builds_incompatible,
 * ]
 */
import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {waitForState} from "jstests/replsets/rslib.js";

const rst = new ReplSetTest({nodes: [{}, {}, {arbiter: true}]});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const dbName = jsTestName();
const collName = "coll";
const indexName = "a_1";

if (!FeatureFlagUtil.isPresentAndEnabled(primary.getDB(dbName), "PrimaryDrivenIndexBuilds")) {
    jsTest.log.info("Skipping: featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}

jsTest.log.info("1. Seed data on primary");
assert.commandWorked(primary.getDB(dbName).createCollection(collName));
const primaryColl = primary.getDB(dbName).getCollection(collName);
for (let i = 0; i < 10; i++) {
    assert.commandWorked(primaryColl.insert({a: i}));
}
rst.awaitReplication();

jsTest.log.info("2. Pause index build before signalling commit readiness");
const hangBeforeSignal = configureFailPoint(primary, "hangIndexBuildBeforeSignalPrimaryForCommitReadiness");

jsTest.log.info("3. Start index build on primary");
const awaitIndexBuild = IndexBuildTest.startIndexBuild(
    primary,
    primaryColl.getFullName(),
    {a: 1},
    {name: indexName},
    [ErrorCodes.InterruptedDueToReplStateChange],
);
hangBeforeSignal.wait();

jsTest.log.info("4. Stop original primary; secondary will step up and abort the PDIB");
rst.stop(primary, undefined, {forRestart: true, skipValidation: true});
awaitIndexBuild({checkExitSuccess: false});

jsTest.log.info("5. Wait for secondary to step up");
waitForState(secondary, ReplSetTest.State.PRIMARY);
const newPrimary = rst.getPrimary();
assert.eq(newPrimary.host, secondary.host);

jsTest.log.info("6. Restart the original primary as a secondary");
const restartedPrimaryId = rst.getNodeId(primary);
rst.start(restartedPrimaryId, {}, true /* restart */);
rst.awaitNodesAgreeOnPrimary();

jsTest.log.info("7. Pause the next step-up abort right before WUOW commit, then force stepdown");
// hangBeforeCompletingAbort fires inside the abort path while the WUOW is open. Forcing a
// stepdown here interrupts the operation, causing the WUOW to roll back. With the pre-fix
// behaviour (addDropPendingIdent outside onCommit), the rolled-back abort still leaves the
// ident registered with the reaper.
const hangBeforeAbort = configureFailPoint(newPrimary, "hangBeforeCompletingAbort");

jsTest.log.info("8. Force a fresh PDIB on the new primary so the abort path engages again");
const hangBeforeSignal2 = configureFailPoint(newPrimary, "hangIndexBuildBeforeSignalPrimaryForCommitReadiness");
const awaitSecondBuild = IndexBuildTest.startIndexBuild(
    newPrimary,
    primaryColl.getFullName(),
    {a: 1},
    {name: indexName},
    [ErrorCodes.InterruptedDueToReplStateChange, ErrorCodes.IndexBuildAborted],
);
hangBeforeSignal2.wait();

jsTest.log.info("9. Step down the new primary while the build is paused; this enqueues a PDIB abort");
rst.stop(newPrimary, undefined, {forRestart: true, skipValidation: true});
awaitSecondBuild({checkExitSuccess: false});

waitForState(rst.nodes[restartedPrimaryId], ReplSetTest.State.PRIMARY);

jsTest.log.info("10. Restart the formerly-new primary; it will re-enter the step-up abort path");
rst.start(rst.getNodeId(newPrimary), {}, true /* restart */);
rst.awaitNodesAgreeOnPrimary();
hangBeforeAbort.off();
hangBeforeSignal2.off();

jsTest.log.info("11. Verify cluster is healthy and no node crashed on the reaper invariant");
rst.awaitReplication();
const finalPrimary = rst.getPrimary();
IndexBuildTest.assertIndexes(finalPrimary.getDB(dbName).getCollection(collName), 1, ["_id_"]);

jsTest.log.info("12. Re-create the index successfully to confirm idents are reusable");
assert.commandWorked(finalPrimary.getDB(dbName).getCollection(collName).createIndex({a: 1}, {name: indexName}));
IndexBuildTest.assertIndexes(finalPrimary.getDB(dbName).getCollection(collName), 2, ["_id_", indexName]);

rst.stopSet();
