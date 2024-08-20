/**
 * Tests that a user write that is waiting on the journal flusher thread can be interrupted.
 *
 * @tags: [
 *     requires_journaling,
 *     requires_latch_analyzer,
 *     requires_replication,
 *     requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet({journalCommitInterval: 500});
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const testColl = testDB.getCollection('t');
assert.commandWorked(testDB.createCollection(testColl.getName()));

primary.setLogLevel(1, 'storage');

// Pause the JournalFlusher thread right before it flushes. While the thread is blocked on the
// fail point, we start a user write with an majority write concern that will block.
let journalFlusherFP = configureFailPoint(primary, "pauseJournalFlusherBeforeFlush");

let runInsert;
let doc = {_id: 0};
try {
    journalFlusherFP.wait();

    runInsert = startParallelShell(
        funWithArgs((collName, doc) => {
            jsTestLog('Inserting document ' + tojson(doc) + ' into ' + collName);
            const coll = db.getMongo().getCollection(collName);
            const result = assert.commandFailedWithCode(
                coll.insert(doc, {writeConcern: {w: "majority", j: true}}), ErrorCodes.Interrupted);
            jsTestLog('Insert operation failed (as expected) with result: ' + tojson(result));
        }, testColl.getFullName(), doc), primary.port);

    // Find the insert thread's opID.
    jsTestLog('Looking for insert operation in currentOp results.');
    let op;
    assert.soon(() => {
        const currentOpResults = testDB.currentOp({
            op: 'insert',
            ns: testColl.getFullName(),
            connectionId: {$exists: true},
            waitingForLatch: {$exists: true}
        });
        jsTestLog("currentOp results: " + tojson(currentOpResults));
        if (currentOpResults.inprog.length == 1) {
            op = currentOpResults.inprog[0];
            return true;
        }
        return false;
    });
    jsTestLog('Found insert operation in currentOp results: ' + tojson(op));

    // Try to kill the JournalFlusher thread.
    jsTestLog('Interrupting insert operation with opId: ' + op.opid);
    assert.commandWorked(testDB.killOp(op.opid));

    // Wait for the insert operation to go away to confirm that the journal flusher wait
    // was interrupted successfully.
    jsTestLog('Waiting for interrupted insert operation to complete.');
    assert.soon(() => {
        const currentOpResults = testDB.currentOp({
            op: 'insert',
            ns: testColl.getFullName(),
            connectionId: {$exists: true},
        });
        jsTestLog("currentOp results: " + tojson(currentOpResults));
        return currentOpResults.inprog.length == 0;
    });
    jsTestLog('Interrupted insert operation is gone. Test successful.');
} finally {
    // Ensure the failpoint is turned off so the stepdown command waits for a data flush.
    journalFlusherFP.off();
}
runInsert();

rst.stopSet();
