/**
 * TODO: Rename test to remove the word "update"
 * This test checks that an update operation does not hold write locks while sleeping for backoff after a
 * write conflict.
 * @tags: [requires_fcv_83]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {Thread} from "jstests/libs/parallelTester.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Use a fixed number of execution control tickets so that we can easily force
            // operations to compete for tickets.
            executionControlConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
        },
    },
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("test");
const coll = db.concurrent_update_test;

// Insert test documents
const numDocs = 1050;
assert.commandWorked(
    coll.insertMany(
        [...Array(numDocs).keys()].map((x) => ({_id: x, a: "foo", b: x})),
        {ordered: false},
    ),
);

// Enable the failpoint to hang before logging and backoff.
const hangFp = configureFailPoint(primary, "planExecutorHangBeforeLogAndBackoff");

// Enable the failpoint to throw write conflicts during batched delete.
const writeConflictFp = configureFailPoint(primary, "throwWriteConflictExceptionInBatchedDeleteStage");

// Create a thread that will run a batched delete.
const deleteThread = new Thread(function (host) {
    const conn = new Mongo(host);
    const db = conn.getDB("test");
    const coll = db.concurrent_update_test;

    // Run a batched delete operation.
    const result = db.runCommand({
        delete: coll.getName(),
        deletes: [{q: {a: "foo"}, limit: 0}],
    });

    // TODO: SERVER-116168 We should also do this test with an express update.

    return result;
}, primary.host);

deleteThread.start();

// Wait until the failpoint has been hit.
hangFp.wait();

jsTestLog("Batched delete thread has hit the planExecutorHangBeforeLogAndBackoff failpoint");

// Set concurrent write transactions to 1 to force other writes to compete for tickets. If the delete
// thread is holding write tickets then there should be no available write tickets.
assert.commandWorked(
    primary.adminCommand({
        setParameter: 1,
        executionControlConcurrentWriteTransactions: 1,
    }),
);

// Attempt to do a write. If this doesn't block it means the delete thread is not holding write tickets.
assert.commandWorked(
    db.runCommand({
        insert: coll.getName(),
        documents: [{_id: numDocs + 1, a: "bar", b: 100}],
        maxTimeMS: 10000,
    }),
);

jsTestLog("Successfully inserted document, confirming delete thread is not holding write tickets");

// Disable the failpoints and allow the delete operation to finish.
hangFp.off();
writeConflictFp.off();
jsTestLog("Failpoints disabled, allowing blocked delete operation to proceed");

// Wait for the delete thread to complete.
deleteThread.join();
const result = deleteThread.returnData();
jsTest.log("Delete thread completed with result: " + tojson(result));

// Cleanup.
rst.stopSet();
