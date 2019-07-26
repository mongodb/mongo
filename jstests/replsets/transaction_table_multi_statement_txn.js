/**
 * Tests that the transaction table is properly updated on secondaries after committing a
 * transaction.
 *
 * @tags: [uses_transactions]
 */
(function() {
'use strict';

load("jstests/libs/retryable_writes_util.js");

const replTest = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}, {arbiter: true}]});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();
const session = primary.startSession();
const primaryDB = session.getDatabase('test');
const coll = primaryDB.getCollection('coll');

jsTestLog('Creating collection ' + coll.getFullName());
assert.commandWorked(primaryDB.createCollection(coll.getName(), {writeConcern: {w: "majority"}}));
replTest.awaitReplication();

const sessionId = session.getSessionId();
jsTestLog('Starting transaction on session ' + sessionId);
session.startTransaction();
assert.writeOK(coll.insert({_id: 0}));
assert.writeOK(coll.insert({_id: 1}));
assert.commandWorked(session.commitTransaction_forTesting());
const opTime = session.getOperationTime();
const txnNum = session.getTxnNumber_forTesting();
jsTestLog('Successfully committed transaction at operation time ' + tojson(opTime) +
          'with transaction number ' + txnNum);

// After replication, assert the secondary's transaction table has been updated.
replTest.awaitReplication();
jsTestLog('Checking transaction tables on both primary and secondary.');
jsTestLog('Primary ' + primary.host + ': ' +
          tojson(primary.getDB('config').transactions.find().toArray()));
jsTestLog('Secondary ' + secondary.host + ': ' +
          tojson(secondary.getDB('config').transactions.find().toArray()));
RetryableWritesUtil.checkTransactionTable(primary, sessionId, txnNum, opTime);
RetryableWritesUtil.assertSameRecordOnBothConnections(primary, secondary, sessionId);

session.endSession();
replTest.stopSet();
})();
