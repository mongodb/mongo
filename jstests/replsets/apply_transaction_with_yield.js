/**
 * SERVER-41222: A TransactionHistoryIterator being used to apply a transaction on a secondary
 * should never yield. This test forces the secondary to yield whenever possible and then commits a
 * large transaction. There should be no yields that trigger an invariant failure. This test
 * specifically attempts to trigger the failure referenced in SERVER-41589.
 *
 * @tags: [uses_transactions, requires_majority_read_concern]
 */
(function() {
"use strict";

const name = "change_stream_speculative_majority";
const replTest = new ReplSetTest({name: name, nodes: [{}, {rsConfig: {priority: 0}}]});
replTest.startSet();
replTest.initiate();

const dbName = name;
const collName = "coll";

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();

// Collections used in a transaction should be explicitly created first.
assert.commandWorked(primary.getDB(dbName).createCollection(collName));

// Force the secondary to yield at ever opportunity.
assert.commandWorked(
    secondary.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

// Create a transaction that is substantially larger than 16MB, forcing the secondary to apply
// it in multiple batches, so that it uses the TransactionHistoryIterator.
const session = primary.startSession();
session.startTransaction({readConcern: {level: "majority"}});
const sessionColl = session.getDatabase(dbName)[collName];
for (let i = 0; i < 3; i = i + 1) {
    assert.commandWorked(sessionColl.insert({a: 'x'.repeat(15 * 1024 * 1024)}));
}
session.commitTransaction();

// Make sure the transaction has been fully applied.
replTest.awaitReplication();

replTest.stopSet();
})();
