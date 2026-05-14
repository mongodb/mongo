/**
 * Reproduces SERVER-110673: the createIndexes command can return an error to the client before the
 * voteAbortIndexBuild handler thread finishes index metadata cleanup. As a result, an immediately
 * following getIndexes() call may observe stale index metadata for the failed build.
 *
 * Race (two-phase abort path):
 *   1. Builder thread hits a DuplicateKey during the collection scan and enters the two-phase
 *      abort path, sending 'voteAbortIndexBuild' to itself.
 *   2. The handler receives the loopback on a separate connection, calls tryAbort() which wakes
 *      the builder thread, then begins index metadata cleanup.
 *   3. The builder thread returns up the stack without taking further locks and sets
 *      sharedPromise.setError() before the handler removes the index from the catalog.
 *   4. createIndexes unblocks on the promise and returns the error to the client while the
 *      handler is still mid-cleanup. A racing getIndexes() sees the still-registered index.
 *
 * The repro pins step (3)/(4) ahead of (2)'s catalog mutation by hanging the handler at
 * 'hangBeforeCompletingAbort' (which fires after tryAbort wakes the builder, before
 * _completeExternalAbort removes metadata). With the failpoint active, createIndexes is expected
 * to return error before cleanup -- the bug surfaces as a non-soon getIndexes() showing the index
 * still present. After releasing the failpoint, cleanup completes and getIndexes() converges.
 *
 * @tags: [
 *   # The race lives on the two-phase abort path; primary-driven builds bypass it (SERVER-113823).
 *   requires_commit_quorum,
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on the secondary so the abort stays primary-side.
            rsConfig: {priority: 0},
        },
    ],
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB("test");
const primaryColl = primaryDB.getCollection(jsTestName());

primaryColl.drop();

// Insert documents that violate a future unique index on {a: 1}. The collection scan phase will
// hit a DuplicateKey error during key insertion, driving the builder into the two-phase
// self-abort path that loops 'voteAbortIndexBuild' back to this node.
assert.commandWorked(primaryColl.insert([{a: 1}, {a: 1}]));

// Pin the bug window: hang the voteAbortIndexBuild handler AFTER it has called tryAbort() and
// woken the builder thread, but BEFORE _completeExternalAbort() removes the index from the
// durable catalog. This is the exact gap where the builder's sharedPromise.setError() can race
// ahead of metadata cleanup.
const hangBeforeCompletingAbort = configureFailPoint(primaryDB, "hangBeforeCompletingAbort");

jsTestLog("Starting index build that will self-abort with DuplicateKey");
const createIdx = IndexBuildTest.startIndexBuild(
    primary,
    primaryColl.getFullName(),
    {a: 1},
    {unique: true},
    [ErrorCodes.DuplicateKey],
    /*commitQuorum=*/ 2,
);

// Wait until the handler has reached the pinned point. At this instant the builder thread is
// already unblocked and on its way to setting sharedPromise.setError(); createIndexes will
// return error to the client without waiting for the handler's catalog cleanup.
hangBeforeCompletingAbort.wait();

// createIndexes returns to the parallel shell -- the client now sees the failure.
createIdx();

// Bug assertion: with the handler still stuck pre-cleanup, the builder's promise has already
// settled, but the failed index has NOT been removed from the catalog yet. A non-soon
// getIndexes() should expose the stale registration. This is the assertion that flakes on a
// buggy server; once SERVER-110673 is fixed, createIndexes will not return until the handler
// has completed cleanup, so only ["_id_"] should be visible here.
const indexesWhileHandlerStuck = primaryColl.getIndexes().map((spec) => spec.name);
jsTestLog("Indexes observed immediately after createIndexes returned: " + tojson(indexesWhileHandlerStuck));
assert.eq(
    [_idIndexName()],
    indexesWhileHandlerStuck.sort(),
    "SERVER-110673: createIndexes returned to client before voteAbortIndexBuild handler " +
        "completed index metadata cleanup; getIndexes() observed stale index registration: " +
        tojson(indexesWhileHandlerStuck),
);

// Release the handler to let cleanup finish; the index must eventually disappear.
hangBeforeCompletingAbort.off();
IndexBuildTest.assertIndexesSoon(primaryColl, 1, ["_id_"], []);

rst.stopSet();

function _idIndexName() {
    return "_id_";
}
