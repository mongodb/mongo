/**
 * Tests that the logical session cache reaper would only reap the config.transactions and
 * config.image_collection entries for a transaction session if the logical session that it
 * corresponds to has expired and been removed from the config.system.sessions collection.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */

(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

// This test runs the reapLogicalSessionCacheNow command. That can lead to direct writes to the
// config.transactions collection, which cannot be performed on a session.
TestData.disableImplicitSessions = true;

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            maxSessions: 1,
            // Make transaction records expire immediately.
            TransactionRecordMinimumLifetimeMinutes: 0,
            storeFindAndModifyImagesInSideCollection: true,
            internalSessionsReapThreshold: 0
        }
    }
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const kConfigSessionsNs = "config.system.sessions";
const kConfigTxnsNs = "config.transactions";
const kImageCollNs = "config.image_collection";
const kOplogCollNs = "local.oplog.rs";
const sessionsColl = primary.getCollection(kConfigSessionsNs);
const transactionsColl = primary.getCollection(kConfigTxnsNs);
const imageColl = primary.getCollection(kImageCollNs);
const oplogColl = primary.getCollection(kOplogCollNs);

const dbName = "testDb";
const collName = "testColl";
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testDB.runCommand({
    insert: collName,
    documents: [{_id: 0}, {_id: 1}],
}));

const sessionUUID = UUID();
const parentLsid = {
    id: sessionUUID
};
const parentLsidFilter = makeLsidFilter(parentLsid, "_id");
let parentTxnNumber = 0;
const childTxnNumber = NumberLong(0);

let numTransactionsCollEntriesReaped = 0;

{
    jsTest.log("Test reaping when there is an expired internal transaction session for a " +
               "non-retryable write without an open transaction");

    parentTxnNumber++;
    assert.commandWorked(testDB.runCommand({
        findAndModify: collName,
        query: {_id: 0},
        update: {$set: {x: 0}},
        lsid: parentLsid,
        txnNumber: NumberLong(parentTxnNumber),
    }));
    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.eq(1, sessionsColl.find({"_id.id": sessionUUID}).itcount());
    assert.eq(1, transactionsColl.find(parentLsidFilter).itcount());
    assert.eq(1, imageColl.find(parentLsidFilter).itcount());

    const childLsid = {id: sessionUUID, txnUUID: UUID()};
    const childLsidFilter = makeLsidFilter(childLsid, "_id");
    assert.commandWorked(testDB.runCommand({
        findAndModify: collName,
        query: {_id: 0},
        update: {$set: {y: 0}},
        lsid: childLsid,
        txnNumber: childTxnNumber,
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(
        testDB.adminCommand(makeCommitTransactionCmdObj(childLsid, childTxnNumber)));

    assert.eq({_id: 0, x: 0, y: 0}, testColl.findOne({_id: 0}));

    // Verify that the config.transactions entry for the internal transaction session does not get
    // reaped automatically when the transaction committed.
    assert.eq(1, sessionsColl.find({"_id.id": sessionUUID}).itcount());
    assert.eq(1, transactionsColl.find(parentLsidFilter).itcount());
    assert.eq(1, transactionsColl.find(childLsidFilter).itcount());
    assert.eq(1, imageColl.find(parentLsidFilter).itcount());
    assert.eq(0, imageColl.find(childLsidFilter).itcount());

    // Force the logical session cache to reap, and verify that the config.transactions (and
    // config.image_collection) entries for both transaction sessions do not get reaped because the
    // config.system.sessions entry still has not been deleted.
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assert.eq(1, sessionsColl.find({"_id.id": sessionUUID}).itcount());
    assert.eq(1, transactionsColl.find(parentLsidFilter).itcount());
    assert.eq(1, transactionsColl.find(childLsidFilter).itcount());
    assert.eq(1, imageColl.find(parentLsidFilter).itcount());
    assert.eq(0, imageColl.find(childLsidFilter).itcount());

    // Delete the config.system.sessions entry, force the logical session cache to reap again, and
    // verify that the config.transactions (and config.image_collection) entries for both sessions
    // do get reaped this time.
    assert.commandWorked(sessionsColl.remove({}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assert.eq(0, sessionsColl.find({"_id.id": sessionUUID}).itcount());
    assert.eq(0, transactionsColl.find(parentLsidFilter).itcount());
    assert.eq(0, transactionsColl.find(childLsidFilter).itcount());
    assert.eq(0, imageColl.find(parentLsidFilter).itcount());
    assert.eq(0, imageColl.find(childLsidFilter).itcount());
    numTransactionsCollEntriesReaped += 2;
}

{
    jsTest.log("Test reaping when there is an expired internal transaction session for a " +
               "previous retryable write (i.e. with an old txnNumber)");

    parentTxnNumber++;
    assert.commandWorked(testDB.runCommand({
        findAndModify: collName,
        query: {_id: 1},
        update: {$set: {x: 1}},
        lsid: parentLsid,
        txnNumber: NumberLong(parentTxnNumber),
    }));
    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.eq(1, sessionsColl.find({"_id.id": sessionUUID}).itcount());
    assert.eq(1, transactionsColl.find(parentLsidFilter).itcount());
    assert.eq(1, imageColl.find(parentLsidFilter).itcount());

    parentTxnNumber++;
    const childLsid = {id: sessionUUID, txnNumber: NumberLong(parentTxnNumber), txnUUID: UUID()};
    const childLsidFilter = makeLsidFilter(childLsid, "_id");
    assert.commandWorked(testDB.runCommand({
        findAndModify: collName,
        query: {_id: 1},
        update: {$set: {y: 1}},
        lsid: childLsid,
        txnNumber: childTxnNumber,
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(
        testDB.adminCommand(makeCommitTransactionCmdObj(childLsid, childTxnNumber)));

    assert.eq({_id: 1, x: 1, y: 1}, testColl.findOne({_id: 1}));

    parentTxnNumber++;
    assert.commandWorked(testDB.runCommand({
        findAndModify: collName,
        query: {_id: 1},
        update: {$set: {y: 1}},
        lsid: parentLsid,
        txnNumber: NumberLong(parentTxnNumber),
        startTransaction: true,
        autocommit: false
    }));

    // Verify that the config.transactions and config.image_collection entries for the internal
    // transaction session do not get reaped automatically when the new txnNumber started since
    // eager reaping is not enabled.
    assert.eq(1, sessionsColl.find({"_id.id": sessionUUID}).itcount());
    assert.eq(1, transactionsColl.find(parentLsidFilter).itcount());
    assert.eq(1, transactionsColl.find(childLsidFilter).itcount());
    assert.eq(1, imageColl.find(parentLsidFilter).itcount());
    assert.eq(1, imageColl.find(childLsidFilter).itcount());

    // Force the logical session cache to reap, and verify that the config.transactions and
    // config.image_collection entries for both transaction sessions do not get reaped because the
    // config.system.sessions entry still has not been deleted.
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assert.eq(1, sessionsColl.find({"_id.id": sessionUUID}).itcount());
    assert.eq(1, transactionsColl.find(parentLsidFilter).itcount());
    assert.eq(1, transactionsColl.find(childLsidFilter).itcount());
    assert.eq(1, imageColl.find(parentLsidFilter).itcount());
    assert.eq(1, imageColl.find(childLsidFilter).itcount());

    assert.commandWorked(
        testDB.adminCommand(makeCommitTransactionCmdObj(parentLsid, parentTxnNumber)));
    assert.eq({_id: 1, x: 1, y: 1}, testColl.findOne({_id: 1}));
    assert.eq(1, sessionsColl.find({"_id.id": sessionUUID}).itcount());
    assert.eq(1, transactionsColl.find(parentLsidFilter).itcount());
    assert.eq(1, transactionsColl.find(childLsidFilter).itcount());
    assert.eq(1, imageColl.find(parentLsidFilter).itcount());
    assert.eq(1, imageColl.find(childLsidFilter).itcount());

    // Force the logical session cache to reap, and verify that the config.transactions and
    // config.image_collection entries for both transaction sessions do not get reaped because the
    // config.system.sessions entry still has not been deleted.
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assert.eq(1, sessionsColl.find({"_id.id": sessionUUID}).itcount());
    assert.eq(1, transactionsColl.find(parentLsidFilter).itcount());
    assert.eq(1, transactionsColl.find(childLsidFilter).itcount());
    assert.eq(1, imageColl.find(parentLsidFilter).itcount());
    assert.eq(1, imageColl.find(childLsidFilter).itcount());

    // Delete the config.system.sessions entry, force the logical session cache to reap again, and
    // verify that the config.transactions and config.image_collection entries for both transaction
    // sessions do get reaped this time.
    assert.commandWorked(sessionsColl.remove({}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assert.eq(0, sessionsColl.find({"_id.id": sessionUUID}).itcount());
    assert.eq(0, transactionsColl.find(parentLsidFilter).itcount());
    assert.eq(0, transactionsColl.find(childLsidFilter).itcount());
    assert.eq(0, imageColl.find(parentLsidFilter).itcount());
    assert.eq(0, imageColl.find(childLsidFilter).itcount());
    numTransactionsCollEntriesReaped += 2;
}

// Validate that writes to config.transactions do not generate oplog entries, with the exception of
// deletions.
assert.eq(numTransactionsCollEntriesReaped, oplogColl.find({op: 'd', ns: kConfigTxnsNs}).itcount());
assert.eq(0, oplogColl.find({op: {'$ne': 'd'}, ns: kConfigTxnsNs}).itcount());

rst.stopSet();
})();
