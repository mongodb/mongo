/**
 * Regression test for a deadlock between the ChangeStreamExpiredPreImageRemover background thread
 * and the secondary startup procedure (specifically the RECOVERING -> SECONDARY state transition).
 *
 * The race condition:
 *   1. A secondary restarts and, during startup recovery, reconstructs a prepared transaction.
 *      The prepared transaction placed WiredTiger prepare markers on pre-image documents in
 *      config.system.preimages; those markers remain visible to other readers.
 *   2. The ChangeStreamExpiredPreImageRemover thread starts immediately during startup and begins
 *      performing deletion passes on the pre-images collection. Passes encounter the prepare
 *      markers and block inside wiredTigerPrepareConflictRetrySlow, waiting for the prepared
 *      transaction to commit or abort.
 *   3. Meanwhile, the startup procedure tries to acquire the RSTL in exclusive (X) mode to
 *      transition the node from RECOVERING to SECONDARY.
 *   4. If the remover holds the RSTL in IX mode while the X request is queued, and the remover
 *      then needs to re-acquire a lock already blocked by the X waiter, a deadlock results.
 *
 * The test uses the 'hangBeforeFinishRecovery' failpoint to keep the secondary in RECOVERING state
 * long enough for the remover to execute several passes concurrently with the pending state
 * transition, reproducing the window in which the deadlock can occur.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_persistence,
 *   requires_replication,
 *   uses_transactions,
 *   uses_prepare_transaction,
 * ]
 */
import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";

const testName = jsTestName();

const rst = new ReplSetTest({
    name: testName,
    nodes: 2,
    nodeOptions: {
        setParameter: {
            // Run the expired pre-image removal job very frequently so that many passes happen
            // while the secondary is stuck in RECOVERING state.
            expiredChangeStreamPreImageRemovalJobSleepSecs: 1,
            // Use small truncate markers so the truncate-based deletion path is exercised.
            preImagesCollectionTruncateMarkersMinBytes: 1,
        },
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const testDB = primary.getDB(testName);

const withSkipCollectionAndIndexValidation = (cb) => {
    const previous = TestData.skipCollectionAndIndexValidation;
    TestData.skipCollectionAndIndexValidation = true;
    try {
        return cb();
    } finally {
        TestData.skipCollectionAndIndexValidation = previous;
    }
};

// Create a collection with change stream pre- and post-images enabled.
assert.commandWorked(testDB.createCollection("coll", {changeStreamPreAndPostImages: {enabled: true}}));
const coll = testDB.coll;

// Insert documents and perform several rounds of updates to produce a large number of pre-images.
// Use w:2 to ensure all writes are present on the secondary before we stop it.
const nDocs = 20;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({_id: i, v: 0}, {writeConcern: {w: 2}}));
}
for (let round = 0; round < 10; round++) {
    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(coll.update({_id: i}, {$inc: {v: 1}}, {writeConcern: {w: 2}}));
    }
}

// Disable snapshotting on all nodes. This prevents the stable timestamp from advancing past
// the prepare timestamp, so the prepared transaction is not included in any checkpoint. On
// secondary restart the node must replay the prepare oplog entry to reconstruct the prepared
// transaction during startup recovery, which is the scenario under test.
const disableSnapshottingFPPrimary = configureFailPoint(primary, "disableSnapshotting");
const disableSnapshottingFPSecondary = configureFailPoint(secondary, "disableSnapshotting");

// Open a transaction and update several documents in the preimage-enabled collection. Because
// changeStreamPreAndPostImages is enabled, the transaction writes pre-image records into
// config.system.preimages as part of its transactional data. WiredTiger places prepare markers
// on both the updated documents and those pre-image records. When the secondary reconstructs
// this prepared transaction during startup recovery those markers will be visible to the
// ChangeStreamExpiredPreImageRemover, causing it to block on WT_PREPARE_CONFLICT while
// simultaneously competing for the RSTL with the pending RECOVERING -> SECONDARY transition.
const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(testName);
session.startTransaction({readConcern: {level: "snapshot"}});
for (let i = 0; i < 5; i++) {
    assert.commandWorked(sessionDB.coll.update({_id: i}, {$inc: {v: 1}}));
}

// Use {w: 1} because disableSnapshotting prevents majority write concern from being satisfied.
const prepareTimestamp = PrepareHelpers.prepareTransaction(session, {w: 1});
jsTestLog("Prepared transaction at timestamp: " + tojson(prepareTimestamp));

// Ensure the prepare oplog entry has been replicated to the secondary before stopping it, so
// that the secondary has the prepared transaction state on disk at the time of shutdown.
rst.awaitReplication();

// Disable validation for the shutdown, as it may get stuck on the documents from the prepared
// transaction otherwise.
withSkipCollectionAndIndexValidation(() => {
    jsTestLog("Stopping the secondary");
    rst.stop(secondary, undefined, undefined, {forRestart: true});
});

// Re-enable snapshotting on the primary now that the secondary is stopped. The secondary's
// disableSnapshotting failpoint is automatically cleared when it restarts.
disableSnapshottingFPPrimary.off();

jsTestLog("Restarting the secondary with hangBeforeFinishRecovery failpoint enabled");
const restartedSecondary = rst.start(
    secondary,
    {
        setParameter: {
            "failpoint.hangBeforeFinishRecovery": tojson({mode: "alwaysOn"}),
            "failpoint.preImagesTruncateHangBeforeExecution": tojson({mode: "alwaysOn"}),
            "failpoint.preImagesTruncateHangOnEarlyBailOut": tojson({mode: "alwaysOn"}),
            expiredChangeStreamPreImageRemovalJobSleepSecs: 1,
            preImagesCollectionTruncateMarkersMinBytes: 1,
        },
        waitForConnect: true,
    },
    true /* restart */,
);

jsTestLog("Waiting for secondary to hit the hangBeforeFinishRecovery failpoint");
assert.commandWorked(
    restartedSecondary.adminCommand({
        waitForFailPoint: "hangBeforeFinishRecovery",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);
rst.waitForState(restartedSecondary, ReplSetTest.State.RECOVERING);

// Allow the ChangeStreamExpiredPreImageRemover to execute several passes while the secondary is
// stuck in RECOVERING state with an active reconstructed prepared transaction. The remover will
// repeatedly encounter the prepare markers on pre-image records and block inside
// wiredTigerPrepareConflictRetrySlow, which holds the RSTL in IX mode while waiting for the
// prepared transaction to commit or abort. Simultaneously, releasing the failpoint below will
// cause the startup procedure to request the RSTL in X mode. On a build without a fix this races and
// deadlocks: the X request queues behind the IX holder, IX priority inversion blocks oplog
// application that would commit/abort the prepared transaction, and nothing makes progress.
jsTestLog("Secondary is in RECOVERING state; allowing preimage remover to race with pending RSTL " + "acquisition");

// Wait for the ChangeStreamExpiredPreImageRemover to reach its "hang" failpoint and then make it
// enter the collection scan phase.
assert.commandWorked(
    restartedSecondary.adminCommand({
        waitForFailPoint: "preImagesTruncateHangBeforeExecution",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

assert.commandWorked(
    restartedSecondary.adminCommand({configureFailPoint: "preImagesTruncateHangBeforeExecution", mode: "off"}),
);

jsTestLog("Waiting for preimage remover to early bail out");
assert.commandWorked(
    restartedSecondary.adminCommand({
        waitForFailPoint: "preImagesTruncateHangOnEarlyBailOut",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);
assert.commandWorked(
    restartedSecondary.adminCommand({
        configureFailPoint: "preImagesTruncateHangOnEarlyBailOut",
        mode: "off",
    }),
);

jsTestLog("Releasing hangBeforeFinishRecovery failpoint");
assert.commandWorked(restartedSecondary.adminCommand({configureFailPoint: "hangBeforeFinishRecovery", mode: "off"}));

jsTestLog("Waiting for secondary to fully transition to SECONDARY state");
rst.awaitSecondaryNodes(ReplSetTest.kDefaultTimeoutMS, [restartedSecondary]);

// Abort the prepared transaction on the primary. The secondary will apply the abort oplog
// entry during normal secondary oplog application.
jsTestLog("Aborting the prepared transaction");
assert.commandWorked(session.abortTransaction_forTesting());

rst.awaitReplication();

rst.stopSet();
