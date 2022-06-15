/**
 * Tests that killOp is ineffectual against the journal flusher thread.
 *
 * @tags: [requires_replication]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet({journalCommitInterval: 500});
rst.initiate();

const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");
const testColl = primary.getDB("testDB").getCollection("testColl");

// Pause the JournalFlusher thread right before it flushes. The JournalFlusher resets its
// OperationContext every flush, so we need it to remain stable across identifying the operation via
// currentOp (which looks at the opCtx ID for opId) and then marking it to die via killOp (by opId).
let journalFlusherFP = configureFailPoint(primary, "pauseJournalFlusherBeforeFlush");

try {
    journalFlusherFP.wait();

    // Find the JournalFlusher thread's opID.
    const currentOpResults = adminDB.currentOp();
    const journalFlusherOp = currentOpResults.inprog.filter(function(op) {
        if (op.desc && op.desc == "JournalFlusher") {
            jsTestLog("Found JournalFlusher operation: " + tojson(op));
            return true;
        }
    });
    assert.eq(1,
              journalFlusherOp.length,
              "Unexpectedly found multiple JournalFlusher operations: " + tojson(journalFlusherOp));

    // Try to kill the JournalFlusher thread.
    assert.commandWorked(adminDB.killOp(journalFlusherOp[0].opid));
} finally {
    // Ensure the failpoint is turned off so the server cannot hang on shutdown.
    journalFlusherFP.off();
}

// Whenever the journal flusher tries to run, it should encounter the killOp Interrupt error.
checkLog.containsJson(primary, 5574501);

rst.stopSet();
// MongoRunner.stopMongod(conn);
})();
