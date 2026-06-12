/**
 * This test checks that write operations do not hold write locks while sleeping for backoff after a
 * write conflict. Specifically, it tests:
 * 1. A batched delete (classic executor) does not hold write tickets while sleeping.
 * 2. An express update does not hold write tickets while sleeping.
 * 3. An express delete does not hold write tickets while sleeping.
 * 4. An express update DOES hold the write ticket when the MaxReleaseTicketCycles fallback
 *    threshold is crossed.
 * 5. An express update does not hold write tickets while sleeping for TemporarilyUnavailable
 *    backoff (WaitingForBackoff path).
 * @tags: [requires_fcv_83]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {isExpress} from "jstests/libs/query/analyze_plan.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {Thread} from "jstests/libs/parallelTester.js";

/**
 * Drives one scenario end-to-end on a fresh ReplSet. When expectTicketHeld is false the
 * concurrent insert succeeds (the delete thread released its ticket before sleeping); when
 * expectTicketHeld is true the concurrent insert is expected to time out (the delete thread
 * still holds the ticket during the sleep).
 */
function runScenario({maxReleaseCycles, expectTicketHeld}) {
    jsTestLog(
        `Running scenario: maxReleaseCycles=${maxReleaseCycles} expectTicketHeld=${expectTicketHeld}`,
    );

    const rst = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            setParameter: {
                // Fixed ticket count so we can force writers to compete for the single ticket.
                executionControlConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
                // Defensive: this is the default, but set it explicitly so the test is robust
                // against future default flips.
                internalQueryEnableWriteConflictBackoffWithoutTicket: true,
                internalQueryWriteConflictBackoffMaxReleaseTicketCycles: maxReleaseCycles,
            },
        },
    });

    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const db = primary.getDB("test");
    const coll = db.write_conflict_test;

    const numDocs = 1050;
    assert.commandWorked(
        coll.insertMany(
            [...Array(numDocs).keys()].map((x) => ({_id: x, a: "foo", b: x})),
            {ordered: false},
        ),
    );

    // Verify the batched delete does NOT use the express executor.
    {
        const explainCmd = {
            explain: {delete: coll.getName(), deletes: [{q: {a: "foo"}, limit: 0}]},
            verbosity: "queryPlanner",
        };
        const explain = assert.commandWorked(db.runCommand(explainCmd));
        assert(!isExpress(db, explain), "Batched delete should not use express executor", {
            explain,
        });
    }

    // Hang before logAndBackoff so we can probe ticket state while the delete is suspended.
    const hangFp = configureFailPoint(primary, "planExecutorHangBeforeLogAndBackoff");
    // Force the delete stage to throw WriteConflict on every attempt.
    const writeConflictFp = configureFailPoint(
        primary,
        "throwWriteConflictExceptionInBatchedDeleteStage",
    );

    const deleteThread = new Thread(function (host) {
        const conn = new Mongo(host);
        const db = conn.getDB("test");
        const coll = db.write_conflict_test;

        return db.runCommand({
            delete: coll.getName(),
            deletes: [{q: {a: "foo"}, limit: 0}],
        });
    }, primary.host);

    deleteThread.start();
    hangFp.wait();
    jsTestLog("Batched delete thread has hit the planExecutorHangBeforeLogAndBackoff failpoint");

    // Only one write ticket available from now on. Whether the delete thread holds it or not
    // is what this test is verifying.
    assert.commandWorked(
        primary.adminCommand({
            setParameter: 1,
            executionControlConcurrentWriteTransactions: 1,
        }),
    );

    // Probe with a concurrent insert under a short deadline.
    const insertRes = db.runCommand({
        insert: coll.getName(),
        documents: [{_id: numDocs + 1, a: "bar", b: 100}],
        maxTimeMS: 30000,
    });

    if (expectTicketHeld) {
        // With M=0 the delete falls back to the legacy hold-ticket path. The insert must time
        // out because the sole write ticket is held by the hanging delete.
        assert.commandFailedWithCode(
            insertRes,
            ErrorCodes.MaxTimeMSExpired,
            "insert was expected to time out while the delete held the write ticket",
        );
        jsTestLog("Insert timed out as expected -- delete thread is holding the write ticket");
    } else {
        // With the default (M = INT_MAX) the delete releases its ticket before sleeping, so
        // the concurrent insert should get the ticket promptly.
        assert.commandWorked(insertRes);
        jsTestLog("Insert succeeded -- delete thread released the write ticket");
    }

    hangFp.off();
    writeConflictFp.off();

    deleteThread.join();
    jsTest.log("Delete thread completed with result: " + tojson(deleteThread.returnData()));

    rst.stopSet();
}

// Default behavior: release ticket before sleep (M = INT_MAX).
runScenario({maxReleaseCycles: 2147483647, expectTicketHeld: false});

// Fallback behavior: M = 0 forces hold-ticket + sleep from the first WCE.
runScenario({maxReleaseCycles: 0, expectTicketHeld: true});

// =====================================================================================
// Tests 2-5: Express executor tests on a shared ReplSet.
// =====================================================================================

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Use a fixed number of execution control tickets so that we can easily force
            // operations to compete for tickets.
            executionControlConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
            // The fix for this is gated behind a flag since it's still experimental.
            internalQueryEnableWriteConflictBackoffWithoutTicket: true,
        },
    },
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("test");
const coll = db.write_conflict_test;

// Insert test documents
const numDocs = 1050;
assert.commandWorked(
    coll.insertMany(
        [...Array(numDocs).keys()].map((x) => ({_id: x, a: "foo", b: x})),
        {ordered: false},
    ),
);

// =====================================================================================
// Test 2: Express executor does not hold write tickets while sleeping for backoff.
// =====================================================================================

// Set an explicit ticket limit so tests start from a known state.
assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        executionControlConcurrentWriteTransactions: 100,
    }),
);

// Insert a fresh document for the express update test.
const expressTestDocId = numDocs + 10;
assert.commandWorked(coll.insertOne({_id: expressTestDocId, a: "foo", b: 10}));

// Verify the update-by-_id DOES use the express executor.
{
    const explainCmd = {
        explain: {
            update: coll.getName(),
            updates: [{q: {_id: expressTestDocId}, u: {$set: {a: "bar"}}}],
        },
        verbosity: "queryPlanner",
    };
    const explain = assert.commandWorked(db.runCommand(explainCmd));
    assert(isExpress(db, explain), "Update by _id should use express executor: " + tojson(explain));
}

// Enable the failpoint to hang before logging and backoff in the express executor.
const expressHangFp = configureFailPoint(primary, "expressExecutorHangBeforeLogAndBackoff");

// Enable the failpoint to throw write conflicts during express writes.
const expressWriteConflictFp = configureFailPoint(
    primary,
    "throwWriteConflictExceptionInExpressWrite",
);

// Create a thread that will run an express update (single-doc update by _id).
const expressUpdateThread = new Thread(
    function (host, docId) {
        const conn = new Mongo(host);
        const db = conn.getDB("test");
        const coll = db.write_conflict_test;

        // Update by _id so the express executor is chosen.
        const result = db.runCommand({
            update: coll.getName(),
            updates: [{q: {_id: docId}, u: {$set: {a: "bar"}}}],
        });

        return result;
    },
    primary.host,
    expressTestDocId,
);

expressUpdateThread.start();

// Wait until the express executor hits the pre-backoff failpoint.
expressHangFp.wait();

jsTestLog("Express update thread has hit the expressExecutorHangBeforeLogAndBackoff failpoint");

// Set concurrent write transactions to 1 to force other writes to compete for tickets. If the
// express thread is holding write tickets while sleeping, there will be no tickets available.
assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        executionControlConcurrentWriteTransactions: 1,
    }),
);

// Attempt to do a write. Success means the express thread is not holding write tickets.
assert.commandWorked(
    db.runCommand({
        insert: coll.getName(),
        documents: [{_id: numDocs + 11, a: "bar", b: 200}],
        maxTimeMS: 30000,
    }),
);

jsTestLog("Successfully inserted document, confirming express thread is not holding write tickets");

// Disable the failpoints and allow the express update to finish.
expressHangFp.off();
expressWriteConflictFp.off();
jsTestLog("Failpoints disabled, allowing blocked express operation to proceed");

// Wait for the express thread to complete.
expressUpdateThread.join();
const expressResult = expressUpdateThread.returnData();
jsTest.log("Express update thread completed with result: " + tojson(expressResult));

// =====================================================================================
// Test 3: Express executor does not hold write tickets while sleeping for backoff
//         during a delete.
// =====================================================================================

// Reset the write ticket limit before starting the express delete test.
assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        executionControlConcurrentWriteTransactions: 100,
    }),
);

// Insert a fresh document for the express delete test.
const expressDeleteDocId = numDocs + 20;
assert.commandWorked(coll.insertOne({_id: expressDeleteDocId, a: "foo", b: 30}));

// Verify the delete-by-_id DOES use the express executor.
{
    const explainCmd = {
        explain: {
            delete: coll.getName(),
            deletes: [{q: {_id: expressDeleteDocId}, limit: 1}],
        },
        verbosity: "queryPlanner",
    };
    const explain = assert.commandWorked(db.runCommand(explainCmd));
    assert(isExpress(db, explain), "Delete by _id should use express executor: " + tojson(explain));
}

// Enable the failpoint to hang before logging and backoff in the express executor.
const expressDeleteHangFp = configureFailPoint(primary, "expressExecutorHangBeforeLogAndBackoff");

// Enable the failpoint to throw write conflicts during express writes.
const expressDeleteWriteConflictFp = configureFailPoint(
    primary,
    "throwWriteConflictExceptionInExpressWrite",
);

// Create a thread that will run an express delete (single-doc delete by _id).
const expressDeleteThread = new Thread(
    function (host, docId) {
        const conn = new Mongo(host);
        const db = conn.getDB("test");
        const coll = db.write_conflict_test;

        // Delete by _id so the express executor is chosen.
        const result = db.runCommand({
            delete: coll.getName(),
            deletes: [{q: {_id: docId}, limit: 1}],
        });

        return result;
    },
    primary.host,
    expressDeleteDocId,
);

expressDeleteThread.start();

// Wait until the express executor hits the pre-backoff failpoint.
expressDeleteHangFp.wait();

jsTestLog("Express delete thread has hit the expressExecutorHangBeforeLogAndBackoff failpoint");

// Set concurrent write transactions to 1. If the express thread is holding write tickets while
// sleeping, there will be no tickets available for the concurrent write below.
assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        executionControlConcurrentWriteTransactions: 1,
    }),
);

// Attempt to do a write. Success means the express delete thread is not holding write tickets.
assert.commandWorked(
    db.runCommand({
        insert: coll.getName(),
        documents: [{_id: numDocs + 21, a: "bar", b: 300}],
        maxTimeMS: 30000,
    }),
);

jsTestLog(
    "Successfully inserted document, confirming express delete thread is not holding write tickets",
);

// Disable the failpoints and allow the express delete to finish.
expressDeleteHangFp.off();
expressDeleteWriteConflictFp.off();
jsTestLog("Failpoints disabled, allowing blocked express delete operation to proceed");

// Wait for the express delete thread to complete.
expressDeleteThread.join();
const expressDeleteResult = expressDeleteThread.returnData();
jsTest.log("Express delete thread completed with result: " + tojson(expressDeleteResult));

// =====================================================================================
// Test 4: Express executor HOLDS the write ticket during backoff once the
//         MaxReleaseTicketCycles fallback threshold is crossed.
//
// With maxReleaseTicketCycles=0, any numAttempts > 0 triggers the fallback. The first WCE
// (numAttempts=0) still uses the release-ticket path; the second WCE (numAttempts=1 > 0)
// takes the hold-ticket path. We use skip=1 on the hang failpoint to skip the first
// (release-ticket) activation and catch the thread at the second (hold-ticket) activation,
// then verify that a concurrent write cannot acquire the ticket during that sleep.
// =====================================================================================

// Reset the write ticket limit before starting the fallback test.
assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        executionControlConcurrentWriteTransactions: 100,
    }),
);

// Lower the fallback threshold to 0 so the hold-ticket path is taken on the second WCE.
assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        internalQueryWriteConflictBackoffMaxReleaseTicketCycles: 0,
    }),
);

// Insert a fresh document for the fallback test.
const fallbackTestDocId = numDocs + 30;
assert.commandWorked(coll.insertOne({_id: fallbackTestDocId, a: "foo", b: 40}));

// Verify the update-by-_id uses the express executor.
{
    const explainCmd = {
        explain: {
            update: coll.getName(),
            updates: [{q: {_id: fallbackTestDocId}, u: {$set: {a: "bar"}}}],
        },
        verbosity: "queryPlanner",
    };
    const explain = assert.commandWorked(db.runCommand(explainCmd));
    assert(isExpress(db, explain), "Update by _id should use express executor: " + tojson(explain));
}

// Configure the hang failpoint with skip=1: skip the first activation (release-ticket path at
// numAttempts=0) and only pause at the second activation (hold-ticket path at numAttempts=1).
const fallbackHangFp = configureFailPoint(
    primary,
    "expressExecutorHangBeforeLogAndBackoff",
    {},
    {"skip": 1},
);

// Enable the WCE failpoint so every express write attempt conflicts.
const fallbackWCEFp = configureFailPoint(primary, "throwWriteConflictExceptionInExpressWrite");

// Start an express update thread.
const fallbackThread = new Thread(
    function (host, docId) {
        const conn = new Mongo(host);
        const db = conn.getDB("test");
        const coll = db.write_conflict_test;

        const result = db.runCommand({
            update: coll.getName(),
            updates: [{q: {_id: docId}, u: {$set: {a: "baz"}}}],
        });
        return result;
    },
    primary.host,
    fallbackTestDocId,
);

fallbackThread.start();

// Wait for the second hang activation - the thread is now in the hold-ticket fallback path.
fallbackHangFp.wait();

jsTestLog("Express fallback thread is paused in the hold-ticket backoff path");

// Reduce available tickets to 1. The express thread is holding the only ticket, so any
// concurrent writer must wait.
assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        executionControlConcurrentWriteTransactions: 1,
    }),
);

// A concurrent write should fail to acquire the ticket within maxTimeMS because the express
// thread is holding it while sleeping (the hold-ticket fallback path).
assert.commandFailedWithCode(
    db.runCommand({
        insert: coll.getName(),
        documents: [{_id: numDocs + 31, a: "bar", b: 400}],
        maxTimeMS: 3000,
    }),
    [ErrorCodes.MaxTimeMSExpired, ErrorCodes.LockTimeout],
);

jsTestLog("Concurrent write correctly failed - express fallback thread is holding the ticket");

// Disable the failpoints and allow the express thread to finish.
fallbackHangFp.off();
fallbackWCEFp.off();
jsTestLog("Failpoints disabled, allowing blocked express fallback operation to proceed");

fallbackThread.join();
jsTest.log("Express fallback thread completed with result: " + tojson(fallbackThread.returnData()));

// =====================================================================================
// Test 5: Express executor releases the write ticket while sleeping for
//         TemporarilyUnavailable backoff (WaitingForBackoff path).
// =====================================================================================

// Reset the write ticket limit before starting the test.
assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        executionControlConcurrentWriteTransactions: 100,
    }),
);

// Insert a fresh document for the TemporarilyUnavailable test.
const tuTestDocId = numDocs + 40;
assert.commandWorked(coll.insertOne({_id: tuTestDocId, a: "foo", b: 50}));

// Verify the update-by-_id uses the express executor.
{
    const explainCmd = {
        explain: {
            update: coll.getName(),
            updates: [{q: {_id: tuTestDocId}, u: {$set: {a: "bar"}}}],
        },
        verbosity: "queryPlanner",
    };
    const explain = assert.commandWorked(db.runCommand(explainCmd));
    assert(isExpress(db, explain), "Update by _id should use express executor: " + tojson(explain));
}

// Enable the hang failpoint inside the WaitingForBackoff callback (fires after the ticket is
// released but before the sleep, confirming we are in the released-ticket state).
const tuHangFp = configureFailPoint(
    primary,
    "expressExecutorHangBeforeTemporarilyUnavailableBackoff",
);

// Enable the failpoint that injects a TemporarilyUnavailableException into express writes.
const tuFp = configureFailPoint(primary, "throwTemporarilyUnavailableExceptionInExpressWrite");

// Start an express update thread.
const tuThread = new Thread(
    function (host, docId) {
        const conn = new Mongo(host);
        const db = conn.getDB("test");
        const coll = db.write_conflict_test;

        const result = db.runCommand({
            update: coll.getName(),
            updates: [{q: {_id: docId}, u: {$set: {a: "bar"}}}],
        });
        return result;
    },
    primary.host,
    tuTestDocId,
);

tuThread.start();

// Wait until the express thread is paused inside the WaitingForBackoff callback, at which
// point the ticket has already been released by temporarilyReleaseResourcesAndYield.
tuHangFp.wait();

jsTestLog(
    "Express TemporarilyUnavailable thread has hit the expressExecutorHangBeforeTemporarilyUnavailableBackoff failpoint",
);

// Set concurrent write transactions to 1. If the thread were still holding the ticket this
// insert would time out.
assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        executionControlConcurrentWriteTransactions: 1,
    }),
);

// This insert should succeed because the express thread released its ticket before sleeping.
assert.commandWorked(
    db.runCommand({
        insert: coll.getName(),
        documents: [{_id: numDocs + 41, a: "bar", b: 500}],
        maxTimeMS: 30000,
    }),
);

jsTestLog(
    "Successfully inserted document, confirming express TemporarilyUnavailable thread is not holding write tickets",
);

// Disable the failpoints and allow the express thread to finish.
tuHangFp.off();
tuFp.off();
jsTestLog(
    "Failpoints disabled, allowing blocked express TemporarilyUnavailable operation to proceed",
);

tuThread.join();
jsTest.log(
    "Express TemporarilyUnavailable thread completed with result: " + tojson(tuThread.returnData()),
);

// Cleanup.
rst.stopSet();
