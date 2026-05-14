/**
 * Regression test for SERVER-122159.
 *
 * The FLE2 compact and cleanup commands previously could deadlock with a
 * concurrent setFeatureCompatibilityVersion (setFCV) command. The deadlock
 * cycle reported in the ticket is:
 *
 *   1. setFCV  thread takes MultiDocumentTransactionsBarrier in S mode.
 *   2. compact thread takes the Global lock in IX mode (via AutoGetDb on the
 *      EDC namespace inside the FLE2 command).
 *   3. compact thread's internal transaction tries to take
 *      MultiDocumentTransactionsBarrier in IX mode and blocks on setFCV.
 *   4. setFCV  thread tries to take Global in S and blocks on compact.
 *
 * The FixedFCVRegion taken at the top of compact/cleanup is intended to
 * serialise these two commands; this test exercises the race that occurs if
 * that protection fails. It runs compactStructuredEncryptionData (and
 * separately cleanupStructuredEncryptionData) concurrently with a setFCV
 * downgrade/upgrade and asserts both finish well within a bounded deadline.
 * If the SERVER-122159 deadlock returns, both commands hang on each other
 * and the test fails the deadline assertion.
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_80,
 *   assumes_against_mongod_not_mongos,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {EncryptedClient, isEnterpriseShell} from "jstests/fle2/libs/encrypted_client_util.js";

if (!isEnterpriseShell()) {
    jsTestLog("Skipping FLE2 compact/setFCV deadlock test: requires enterprise shell.");
    quit();
}

// Hard upper bound for each command -- well beyond expected wall clock for
// either command on an idle test cluster, but short enough that a real
// deadlock will trip it long before the evergreen task timeout.
const kDeadlineMs = 60 * 1000;

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");

// Start at latest FCV so the downgrade/upgrade pair has real work to do
// (and so it takes the same locks the bug-trace reports it takes).
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

const dbName = "fle2_compact_setfcv_no_deadlock_db";
const collName = "encrypted";

// Build a QE-enabled encrypted collection on the primary. The
// EncryptedClient wraps a libmongocrypt-aware connection so the compact and
// cleanup commands have their compactionTokens populated automatically when
// run through the encrypted shell.
const client = new EncryptedClient(primary, dbName);
assert.commandWorked(
    client.createEncryptionCollection(collName, {
        encryptedFields: {
            fields: [
                {
                    path: "firstName",
                    bsonType: "string",
                    queries: {queryType: "equality"},
                },
            ],
        },
    }),
);

const edb = client.getDB();
const ecoll = edb.getCollection(collName);

// Insert a few encrypted documents so compact / cleanup actually has work
// to do on the state collections (otherwise the command short-circuits
// before hitting the lock-acquisition window that exhibits the bug).
for (let i = 0; i < 20; ++i) {
    assert.commandWorked(ecoll.einsert({_id: i, firstName: "n" + (i % 5)}));
}

/**
 * Park the FLE2 command at `compactFailpointName` (which fires while compact
 * is holding the Global IX lock + ecoc.lock X lock), then race a setFCV
 * downgrade+upgrade against it.
 *
 * Asserts both commands return within `kDeadlineMs`.
 */
function runConcurrent(label, compactFailpointName, fle2Command) {
    jsTestLog("=== " + label + ": starting concurrent " + fle2Command + " + setFCV ===");

    // Park the FLE2 command in the middle of its locked region.
    const fp = configureFailPoint(primary, compactFailpointName);

    // The FLE2 command runs in a separate parallel shell that constructs its
    // own EncryptedClient -- libmongocrypt fills in the compactionTokens /
    // cleanupTokens automatically.
    const fleShell = startParallelShell(
        funWithArgs(
            async function (dbN, collN, fle2Cmd) {
                const {EncryptedClient} = await import("jstests/fle2/libs/encrypted_client_util.js");
                const c = new EncryptedClient(db.getMongo(), dbN);
                const cmd = {};
                cmd[fle2Cmd] = collN;
                const res = c.getDB().runCommand(cmd);
                assert.commandWorked(res);
            },
            dbName,
            collName,
            fle2Command,
        ),
        primary.port,
    );

    // Wait until the FLE2 command is parked inside its locked region.
    fp.wait();

    // Race a setFCV downgrade against the parked compact in a third shell.
    // Pre-bug, this deadlocks; post-fix, setFCV must either wait cleanly
    // until compact releases, be serialised against compact's FixedFCVRegion,
    // or otherwise resolve without forming a cycle in the wait-for graph.
    const fcvShell = startParallelShell(function () {
        assert.commandWorked(
            db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        assert.commandWorked(
            db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
    }, primary.port);

    // Give setFCV a moment to actually start requesting its barrier / global
    // locks so the wait-for graph would be cyclic if the bug were present.
    sleep(2000);

    // Release the FLE2 failpoint -- both shells must now make progress.
    fp.off();

    // Bounded-time join: if either side is still running past kDeadlineMs
    // the deadlock has regressed.
    const start = Date.now();

    fleShell({checkExitSuccess: true, timeoutMS: kDeadlineMs});
    const fleElapsed = Date.now() - start;
    assert.lt(
        fleElapsed,
        kDeadlineMs,
        label + ": FLE2 " + fle2Command + " did not complete within " + kDeadlineMs +
            " ms -- SERVER-122159 deadlock may have regressed (elapsed=" + fleElapsed + " ms)",
    );

    fcvShell({checkExitSuccess: true, timeoutMS: kDeadlineMs});
    const fcvElapsed = Date.now() - start;
    assert.lt(
        fcvElapsed,
        kDeadlineMs,
        label + ": setFCV did not complete within " + kDeadlineMs +
            " ms -- SERVER-122159 deadlock may have regressed (elapsed=" + fcvElapsed + " ms)",
    );

    jsTestLog("=== " + label + ": done (fle=" + fleElapsed + " ms, setFCV=" + fcvElapsed + " ms) ===");
}

// `fleCompactHangBeforeECOCCreateUnsharded` parks the compact command after
// it has taken Global-IX (via AutoGetDb) + ecoc.lock-X, but before the
// internal-txn that exhibited the SERVER-122159 wait on the
// MultiDocumentTransactionsBarrier -- exactly the window the ticket reports.
runConcurrent("compact", "fleCompactHangBeforeECOCCreateUnsharded", "compactStructuredEncryptionData");

// The corresponding cleanup-side hang failpoint lives in fle2_compact.cpp and
// parks cleanup while it is mid-flight through the internal transaction.
runConcurrent("cleanup", "fleCleanupHangBeforeNullAnchorUpdate", "cleanupStructuredEncryptionData");

rst.stopSet();
