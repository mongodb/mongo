/**
 * Tests that killOp is ineffectual against the journal flusher thread.
 *
 * @tags: [requires_journaling]
 */

(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet({journal: "", journalCommitInterval: 500});
rst.initiate();

const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");
const testColl = primary.getDB("testDB").getCollection("testColl");

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

// Do a write with {j:true} writeConcern to hurry up the next journal flush.
assert.commandWorked(testColl.insert({}, {writeConcern: {j: true}}));

// Whenever the journal flusher tries to run, it should encounter the killOp Interrupt error.
checkLog.containsJson(primary, 5574501);

rst.stopSet();
// MongoRunner.stopMongod(conn);
})();
