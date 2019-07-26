/**
 * Tests that if the writing of the 'commitTransaction' oplog entry advances the 'lastApplied'
 * OpTime before the 'commitTransaction' oplog entry's oplog hole is released, we still move
 * the stable timestamp forward.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
'use strict';
load("jstests/libs/check_log.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const node = rst.getPrimary();

const name = 'hang_before_releasing_transaction_oplog_hole';
const dbName = 'test';
const collName = name;
const testDB = node.getDB(dbName);
const coll = testDB[collName];

// Create collection before running the transaction.
assert.commandWorked(coll.insert({a: 1}));

// Run a transaction in a parallel shell. The transaction will be configured to hang on commit.
// Rather than setting a timeout on commit and forfeiting our ability to check commit for
// success, we use a separate thread to disable the failpoint and allow the server to finish
// committing successfully.
function transactionFn() {
    load('jstests/core/txns/libs/prepare_helpers.js');

    const name = 'hang_before_releasing_transaction_oplog_hole';
    const dbName = 'test';
    const collName = name;
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);

    session.startTransaction({readConcern: {level: 'snapshot'}});
    sessionDB[collName].update({}, {a: 2});
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    // Hang before releasing the 'commitTransaction' oplog entry hole.
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: 'hangBeforeReleasingTransactionOplogHole', mode: 'alwaysOn'}));

    PrepareHelpers.commitTransaction(session, prepareTimestamp);
}
const joinTransaction = startParallelShell(transactionFn, rst.ports[0]);

jsTestLog("Waiting to hang with the oplog hole held open.");
checkLog.contains(node, "hangBeforeReleasingTransactionOplogHole fail point enabled");

jsTestLog("Waiting for 'commitTransaction' to advance lastApplied.");
sleep(5 * 1000);
assert.commandWorked(testDB.adminCommand(
    {configureFailPoint: 'hangBeforeReleasingTransactionOplogHole', mode: 'off'}));

jsTestLog("Joining the transaction.");
joinTransaction();

jsTestLog("Dropping another collection.");
// A w:majority drop on a non-existent collection will not do a write, but will still wait for
// write concern. We double check that that still succeeds.
testDB["otherColl"].drop({writeConcern: {w: "majority"}});

rst.stopSet();
})();
