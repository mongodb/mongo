/*
 * Test that the logical session cache reaper reaps transaction sessions that correspond to the same
 * retryable write atomically.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");

// This test runs the reapLogicalSessionCacheNow command. That can lead to direct writes to the
// config.transactions collection, which cannot be performed on a session.
TestData.disableImplicitSessions = true;

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            TransactionRecordMinimumLifetimeMinutes: 0,
        }
    }
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const kConfigSessionsNs = "config.system.sessions";
const kConfigTxnsNs = "config.transactions";
const kConfigImageNs = "config.image_collection";
const sessionsColl = primary.getCollection(kConfigSessionsNs);
const transactionsColl = primary.getCollection(kConfigTxnsNs);
const imageColl = primary.getCollection(kConfigImageNs);

const kDbName = "testDb";
const kCollName = "testColl";
const kNs = kDbName + "." + kCollName;
const testDB = primary.getDB(kDbName);
const testColl = testDB.getCollection(kCollName);

assert.commandWorked(testDB.createCollection(kCollName));

function makeSessionOptsForTest() {
    const sessionUUID = UUID();
    const parentLsid = {id: sessionUUID};
    const parentTxnNumber = NumberLong(35);
    const childLsidForRetryableWrite = {
        id: sessionUUID,
        txnNumber: parentTxnNumber,
        txnUUID: UUID()
    };
    const childLsidForPrevRetryableWrite = {
        id: sessionUUID,
        txnNumber: NumberLong(parentTxnNumber.valueOf() - 1),
        txnUUID: UUID()
    };
    const childLsidForNonRetryableWrite = {id: sessionUUID, txnUUID: UUID()};
    const childTxnNumber = NumberLong(0);
    return {
        sessionUUID,
        parentLsid,
        parentTxnNumber,
        childLsidForRetryableWrite,
        childLsidForPrevRetryableWrite,
        childLsidForNonRetryableWrite,
        childTxnNumber,
    };
}

function assertNumEntries(
    sessionOpts, {numSessionsCollEntries, numTransactionsCollEntries, numImageCollEntries}) {
    const filter = {"_id.id": sessionOpts.parentLsid.id};

    const sessionsCollEntries = sessionsColl.find(filter).toArray();
    assert.eq(numSessionsCollEntries, sessionsCollEntries.length, sessionsCollEntries);

    const transactionsCollEntries = transactionsColl.find(filter).toArray();
    assert.eq(numTransactionsCollEntries, transactionsCollEntries.length, transactionsCollEntries);

    const imageCollEntries = imageColl.find(filter).toArray();
    assert.eq(numImageCollEntries, imageCollEntries.length, imageCollEntries);
}

// Test reaping when neither the external session nor the internal sessions are checked out.

{
    jsTest.log("Test reaping when there is an in-progress retryable-write internal transaction");
    const sessionOpts = makeSessionOptsForTest();

    assert.commandWorked(testColl.insert([{x: 0, y: 0}]));
    assert.commandWorked(testDB.runCommand({
        findAndModify: kCollName,
        query: {x: 0},
        update: {$inc: {y: 1}},
        new: true,
        lsid: sessionOpts.parentLsid,
        txnNumber: sessionOpts.parentTxnNumber,
        stmtId: NumberInt(0),
    }));

    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 1, numTransactionsCollEntries: 1, numImageCollEntries: 1});

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: sessionOpts.childLsidForRetryableWrite,
        txnNumber: sessionOpts.childTxnNumber,
        startTransaction: true,
        autocommit: false,
        stmtId: NumberInt(1),
    }));

    // Force the logical session cache to reap, and verify that the config.transactions entry and
    // config.image_collection entry for the retryable write in the external session do not get
    // reaped since there is an in-progress internal transaction for that retryable write.
    assert.commandWorked(sessionsColl.remove({"_id.id": sessionOpts.sessionUUID}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 1, numImageCollEntries: 1});

    // Retry the write statement executed in the external session.
    assert.commandWorked(testDB.runCommand({
        findAndModify: kCollName,
        query: {x: 0},
        update: {$inc: {y: 1}},
        lsid: sessionOpts.childLsidForRetryableWrite,
        txnNumber: sessionOpts.childTxnNumber,
        autocommit: false,
        stmtId: NumberInt(0),
    }));
    assert.commandWorked(primary.adminCommand(makeCommitTransactionCmdObj(
        sessionOpts.childLsidForRetryableWrite, sessionOpts.childTxnNumber)));

    // Verify that the retried write statement did not re-execute.
    assert.eq(testColl.find({x: 0, y: 1}).itcount(), 1);
    assert.eq(testColl.find({x: 1}).itcount(), 1);

    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    assert.commandWorked(testColl.remove({}));
}

{
    jsTest.log("Test reaping when there is an in-progress and a committed retryable-write " +
               "internal transaction");
    const sessionOpts = makeSessionOptsForTest();

    assert.commandWorked(testColl.insert([{x: 0, y: 0}]));
    assert.commandWorked(testDB.runCommand({
        findAndModify: kCollName,
        query: {x: 0},
        update: {$inc: {y: 1}},
        new: true,
        lsid: sessionOpts.parentLsid,
        txnNumber: sessionOpts.parentTxnNumber,
        stmtId: NumberInt(0),
    }));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: sessionOpts.childLsidForRetryableWrite,
        txnNumber: sessionOpts.childTxnNumber,
        startTransaction: true,
        autocommit: false,
        stmtId: NumberInt(1),
    }));
    assert.commandWorked(primary.adminCommand(makeCommitTransactionCmdObj(
        sessionOpts.childLsidForRetryableWrite, sessionOpts.childTxnNumber)));

    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 1, numTransactionsCollEntries: 2, numImageCollEntries: 1});

    const runInternalTxn =
        (primaryHost, parentLsidUUIDString, parentTxnNumber, dbName, collName) => {
            load("jstests/sharding/libs/sharded_transactions_helpers.js");

            const primary = new Mongo(primaryHost);
            const testDB = primary.getDB(dbName);

            const childLsid = {
                id: UUID(parentLsidUUIDString),
                txnNumber: NumberLong(parentTxnNumber),
                txnUUID: UUID()
            };
            const childTxnNumber = NumberLong(0);

            assert.commandWorked(testDB.runCommand({
                insert: collName,
                documents: [{x: 2}],
                lsid: childLsid,
                txnNumber: childTxnNumber,
                startTransaction: true,
                autocommit: false,
                stmtId: NumberInt(2),
            }));

            // Retry the write statement executed in the external session.
            assert.commandWorked(testDB.runCommand({
                findAndModify: collName,
                query: {x: 0},
                update: {$inc: {y: 1}},
                new: true,
                lsid: childLsid,
                txnNumber: childTxnNumber,
                autocommit: false,
                stmtId: NumberInt(0),
            }));

            // Retry the write statement executed in the committed internal transaction.
            assert.commandWorked(testDB.runCommand({
                insert: collName,
                documents: [{x: 1}],
                lsid: childLsid,
                txnNumber: childTxnNumber,
                autocommit: false,
                stmtId: NumberInt(1),
            }));

            assert.commandWorked(
                primary.adminCommand(makeCommitTransactionCmdObj(childLsid, childTxnNumber)));
        };

    // Start another internal transaction in a separate thread, and make it hang right after it
    // finishes executing the first statement.
    const fp = configureFailPoint(primary, "waitAfterCommandFinishesExecution", {ns: kNs});
    const internalTxnThread = new Thread(runInternalTxn,
                                         primary.host,
                                         extractUUIDFromObject(sessionOpts.sessionUUID),
                                         sessionOpts.parentTxnNumber.valueOf(),
                                         kDbName,
                                         kCollName);
    internalTxnThread.start();
    fp.wait();

    // Force the logical session cache to reap, and verify that the config.transactions entry and
    // config.image_collection entry for the retryable write in the external session and the
    // config.transactions for the committed internal transaction for that retryable write do not
    // get reaped since there is an in-progress internal transaction for the same retryable write.
    assert.commandWorked(sessionsColl.remove({"_id.id": sessionOpts.sessionUUID}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 2, numImageCollEntries: 1});

    fp.off();
    internalTxnThread.join();

    // Verify that the retried write statements did not re-execute.
    assert.eq(testColl.find({x: 0, y: 1}).itcount(), 1);
    assert.eq(testColl.find({x: 1}).itcount(), 1);
    assert.eq(testColl.find({x: 2}).itcount(), 1);

    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    assert.commandWorked(testColl.remove({}));
}

{
    jsTest.log(
        "Test reaping when there is an in-progress internal transaction for the current retryable" +
        " write and a committed internal transaction for a previous retryable write");
    const sessionOpts = makeSessionOptsForTest();

    assert.commandWorked(testColl.insert([{x: 0, y: 0}]));
    assert.commandWorked(testDB.runCommand({
        findAndModify: kCollName,
        query: {x: 0},
        update: {$inc: {y: 1}},
        new: true,
        lsid: sessionOpts.childLsidForPrevRetryableWrite,
        txnNumber: sessionOpts.childTxnNumber,
        startTransaction: true,
        autocommit: false,
        stmtId: NumberInt(0),
    }));
    assert.commandWorked(primary.adminCommand(makeCommitTransactionCmdObj(
        sessionOpts.childLsidForPrevRetryableWrite, sessionOpts.childTxnNumber)));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: sessionOpts.parentLsid,
        txnNumber: sessionOpts.parentTxnNumber,
        stmtId: NumberInt(1),
    }));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 2}],
        lsid: sessionOpts.childLsidForRetryableWrite,
        txnNumber: sessionOpts.childTxnNumber,
        startTransaction: true,
        autocommit: false,
        stmtId: NumberInt(2),
    }));

    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 1, numTransactionsCollEntries: 2, numImageCollEntries: 1});

    // Force the logical session cache to reap, and verify that the config.transactions entry and
    // config.image_collection entry for the previous write do get reaped.
    assert.commandWorked(sessionsColl.remove({"_id.id": sessionOpts.sessionUUID}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 1, numImageCollEntries: 0});

    assert.commandWorked(primary.adminCommand(makeCommitTransactionCmdObj(
        sessionOpts.childLsidForRetryableWrite, sessionOpts.childTxnNumber)));
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: sessionOpts.parentLsid,
        txnNumber: sessionOpts.parentTxnNumber,
        stmtId: NumberInt(1),
    }));

    assert.eq(testColl.find({x: 0, y: 1}).itcount(), 1);
    assert.eq(testColl.find({x: 1}).itcount(), 1);
    assert.eq(testColl.find({x: 2}).itcount(), 1);

    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    assert.commandWorked(testColl.remove({}));
}

{
    jsTest.log("Test reaping there is an in-progress transaction in the external session and a " +
               "committed internal transaction for a previous retryable write");
    const sessionOpts = makeSessionOptsForTest();

    assert.commandWorked(testColl.insert([{x: 0, y: 0}]));
    assert.commandWorked(testDB.runCommand({
        findAndModify: kCollName,
        query: {x: 0},
        update: {$inc: {y: 1}},
        new: true,
        lsid: sessionOpts.childLsidForPrevRetryableWrite,
        txnNumber: sessionOpts.childTxnNumber,
        startTransaction: true,
        autocommit: false,
        stmtId: NumberInt(0),
    }));
    assert.commandWorked(primary.adminCommand(makeCommitTransactionCmdObj(
        sessionOpts.childLsidForPrevRetryableWrite, sessionOpts.childTxnNumber)));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: sessionOpts.parentLsid,
        txnNumber: sessionOpts.parentTxnNumber,
        startTransaction: true,
        autocommit: false,
    }));

    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 1, numTransactionsCollEntries: 1, numImageCollEntries: 1});

    // Force the logical session cache to reap, and verify that the config.transactions entry and
    // config.image_collection entry for the previous write do get reaped.
    assert.commandWorked(sessionsColl.remove({"_id.id": sessionOpts.sessionUUID}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    assert.commandWorked(primary.adminCommand(
        makeCommitTransactionCmdObj(sessionOpts.parentLsid, sessionOpts.parentTxnNumber)));

    assert.eq(testColl.find({x: 0, y: 1}).itcount(), 1);
    assert.eq(testColl.find({x: 1}).itcount(), 1);

    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    assert.commandWorked(testColl.remove({}));
}

{
    jsTest.log(
        "Test reaping when there is an in-progress non retryable-write internal transaction " +
        "and a committed retryable-write internal transaction");
    const sessionOpts = makeSessionOptsForTest();

    assert.commandWorked(testColl.insert([{x: 0, y: 0}]));
    assert.commandWorked(testDB.runCommand({
        findAndModify: kCollName,
        query: {x: 0},
        update: {$inc: {y: 1}},
        new: true,
        lsid: sessionOpts.childLsidForPrevRetryableWrite,
        txnNumber: sessionOpts.childTxnNumber,
        startTransaction: true,
        autocommit: false,
        stmtId: NumberInt(0),
    }));
    assert.commandWorked(primary.adminCommand(makeCommitTransactionCmdObj(
        sessionOpts.childLsidForPrevRetryableWrite, sessionOpts.childTxnNumber)));

    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 1, numTransactionsCollEntries: 1, numImageCollEntries: 1});

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: sessionOpts.childLsidForNonRetryableWrite,
        txnNumber: sessionOpts.childTxnNumber,
        startTransaction: true,
        autocommit: false,
        stmtId: NumberInt(1),
    }));

    // Force the logical session cache to reap, and verify that the config.transactions entry and
    // config.image_collection entry for the retryable write in external session do not get reaped
    // since there has not been a retryble write or transaction with a higher txnNumber in the
    // logical session.
    assert.commandWorked(sessionsColl.remove({"_id.id": sessionOpts.sessionUUID}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 1, numImageCollEntries: 1});

    assert.commandWorked(primary.adminCommand(makeCommitTransactionCmdObj(
        sessionOpts.childLsidForNonRetryableWrite, sessionOpts.childTxnNumber)));

    assert.eq(testColl.find({x: 0, y: 1}).itcount(), 1);
    assert.eq(testColl.find({x: 1}).itcount(), 1);

    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    assert.commandWorked(testColl.remove({}));
}

{
    jsTest.log("Test reaping when there is an in-progress transaction in the external session " +
               "and a committed non retryable-write internal transaction");
    const sessionOpts = makeSessionOptsForTest();

    assert.commandWorked(testColl.insert([{x: 0, y: 0}]));
    assert.commandWorked(testDB.runCommand({
        findAndModify: kCollName,
        query: {x: 0},
        update: {$inc: {y: 1}},
        new: true,
        lsid: sessionOpts.childLsidForNonRetryableWrite,
        txnNumber: sessionOpts.childTxnNumber,
        startTransaction: true,
        autocommit: false,
        stmtId: NumberInt(0),
    }));
    assert.commandWorked(primary.adminCommand(makeCommitTransactionCmdObj(
        sessionOpts.childLsidForNonRetryableWrite, sessionOpts.childTxnNumber)));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: sessionOpts.parentLsid,
        txnNumber: sessionOpts.parentTxnNumber,
        startTransaction: true,
        autocommit: false,
    }));

    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 1, numTransactionsCollEntries: 1, numImageCollEntries: 0});

    // Force the logical session cache to reap, and verify that the config.transactions entry for
    // the committed non retryable-write internal transaction does get reaped since it is unrelated
    // to the in-progress transaction in the external session.
    assert.commandWorked(sessionsColl.remove({"_id.id": sessionOpts.sessionUUID}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    assert.commandWorked(primary.adminCommand(
        makeCommitTransactionCmdObj(sessionOpts.parentLsid, sessionOpts.parentTxnNumber)));

    assert.eq(testColl.find({x: 0, y: 1}).itcount(), 1);
    assert.eq(testColl.find({x: 1}).itcount(), 1);

    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    assert.commandWorked(testColl.remove({}));
}

// Test reaping when there is a checked out internal session.

{
    jsTest.log("Test reaping when there is a checked out retryable-write internal session with " +
               "an in-progress transaction");
    const sessionOpts = makeSessionOptsForTest();

    assert.commandWorked(testColl.insert([{x: 0, y: 0}]));
    assert.commandWorked(testDB.runCommand({
        findAndModify: kCollName,
        query: {x: 0},
        update: {$inc: {y: 1}},
        new: true,
        lsid: sessionOpts.parentLsid,
        txnNumber: sessionOpts.parentTxnNumber,
        stmtId: NumberInt(0),
    }));

    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 1, numTransactionsCollEntries: 1, numImageCollEntries: 1});

    const runInternalTxn =
        (primaryHost, parentLsidUUIDString, parentTxnNumber, dbName, collName) => {
            load("jstests/sharding/libs/sharded_transactions_helpers.js");

            const primary = new Mongo(primaryHost);
            const testDB = primary.getDB(dbName);

            const childLsid = {
                id: UUID(parentLsidUUIDString),
                txnNumber: NumberLong(parentTxnNumber),
                txnUUID: UUID()
            };
            const childTxnNumber = NumberLong(0);

            assert.commandWorked(testDB.runCommand({
                insert: collName,
                documents: [{x: 1}],
                lsid: childLsid,
                txnNumber: childTxnNumber,
                startTransaction: true,
                autocommit: false,
                stmtId: NumberInt(1),
            }));

            // Retry the write statement executed in the external session.
            assert.commandWorked(testDB.runCommand({
                findAndModify: collName,
                query: {x: 0},
                update: {$inc: {y: 1}},
                new: true,
                lsid: childLsid,
                txnNumber: childTxnNumber,
                autocommit: false,
                stmtId: NumberInt(0),
            }));

            assert.commandWorked(
                primary.adminCommand(makeCommitTransactionCmdObj(childLsid, childTxnNumber)));
        };

    const fp = configureFailPoint(primary, "hangAfterSessionCheckOut", {}, {skip: 1});
    const internalTxnThread = new Thread(runInternalTxn,
                                         primary.host,
                                         extractUUIDFromObject(sessionOpts.sessionUUID),
                                         sessionOpts.parentTxnNumber.valueOf(),
                                         kDbName,
                                         kCollName);
    internalTxnThread.start();
    fp.wait();

    // Force the logical session cache to reap, and verify that the config.transactions entry and
    // config.image_collection entry for the retryable write in the external session do not get
    // reaped.
    assert.commandWorked(sessionsColl.remove({"_id.id": sessionOpts.sessionUUID}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 1, numImageCollEntries: 1});

    fp.off();
    internalTxnThread.join();

    // Verify that the retried write statement did not re-execute.
    assert.eq(testColl.find({x: 0, y: 1}).itcount(), 1);
    assert.eq(testColl.find({x: 1}).itcount(), 1);

    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    assert.commandWorked(testColl.remove({}));
}

{
    jsTest.log("Test reaping when there is a checked out retryable-write internal session " +
               "without an in-progress transaction");
    const sessionOpts = makeSessionOptsForTest();

    assert.commandWorked(testColl.insert([{x: 0, y: 0}]));
    assert.commandWorked(testDB.runCommand({
        findAndModify: kCollName,
        query: {x: 0},
        update: {$inc: {y: 1}},
        new: true,
        lsid: sessionOpts.parentLsid,
        txnNumber: sessionOpts.parentTxnNumber,
        stmtId: NumberInt(0),
    }));

    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 1, numTransactionsCollEntries: 1, numImageCollEntries: 1});

    const runInternalTxn =
        (primaryHost, parentLsidUUIDString, parentTxnNumber, dbName, collName) => {
            load("jstests/sharding/libs/sharded_transactions_helpers.js");

            const primary = new Mongo(primaryHost);
            const testDB = primary.getDB(dbName);

            const childLsid = {
                id: UUID(parentLsidUUIDString),
                txnNumber: NumberLong(parentTxnNumber),
                txnUUID: UUID()
            };
            const childTxnNumber = NumberLong(0);

            assert.commandWorked(testDB.runCommand({
                findAndModify: collName,
                query: {x: 0},
                update: {$inc: {y: 1}},
                new: true,
                lsid: childLsid,
                txnNumber: childTxnNumber,
                startTransaction: true,
                autocommit: false,
                stmtId: NumberInt(0),
            }));
            assert.commandWorked(
                primary.adminCommand(makeCommitTransactionCmdObj(childLsid, childTxnNumber)));
        };

    const fp = configureFailPoint(primary, "hangAfterSessionCheckOut");
    const internalTxnThread = new Thread(runInternalTxn,
                                         primary.host,
                                         extractUUIDFromObject(sessionOpts.sessionUUID),
                                         sessionOpts.parentTxnNumber.valueOf(),
                                         kDbName,
                                         kCollName);
    internalTxnThread.start();
    fp.wait();

    // Force the logical session cache to reap, and verify that the config.transactions entry and
    // config.image_collection entry for the retryable write in the external session do not get
    // reaped.
    assert.commandWorked(sessionsColl.remove({"_id.id": sessionOpts.sessionUUID}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 1, numImageCollEntries: 1});

    fp.off();
    internalTxnThread.join();

    // Verify that the retried write statement did not re-execute.
    assert.eq(testColl.find({x: 0, y: 1}).itcount(), 1);

    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    assert.commandWorked(testColl.remove({}));
}

{
    jsTest.log("Test reaping when there are a checked out retryable-write internal session with " +
               "an in-progress transaction and an unchecked out retryable-write internal " +
               "session for the same retryable write with a committed transaction");
    const sessionOpts = makeSessionOptsForTest();

    assert.commandWorked(testColl.insert([{x: 0, y: 0}]));
    assert.commandWorked(testDB.runCommand({
        findAndModify: kCollName,
        query: {x: 0},
        update: {$inc: {y: 1}},
        new: true,
        lsid: sessionOpts.parentLsid,
        txnNumber: sessionOpts.parentTxnNumber,
        stmtId: NumberInt(0),
    }));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: sessionOpts.childLsidForRetryableWrite,
        txnNumber: sessionOpts.childTxnNumber,
        startTransaction: true,
        autocommit: false,
        stmtId: NumberInt(1),
    }));
    assert.commandWorked(primary.adminCommand(makeCommitTransactionCmdObj(
        sessionOpts.childLsidForRetryableWrite, sessionOpts.childTxnNumber)));

    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 1, numTransactionsCollEntries: 2, numImageCollEntries: 1});

    const runInternalTxn =
        (primaryHost, parentLsidUUIDString, parentTxnNumber, dbName, collName) => {
            load("jstests/sharding/libs/sharded_transactions_helpers.js");

            const primary = new Mongo(primaryHost);
            const testDB = primary.getDB(dbName);

            const childLsid = {
                id: UUID(parentLsidUUIDString),
                txnNumber: NumberLong(parentTxnNumber),
                txnUUID: UUID()
            };
            const childTxnNumber = NumberLong(0);

            assert.commandWorked(testDB.runCommand({
                insert: collName,
                documents: [{x: 2}],
                lsid: childLsid,
                txnNumber: childTxnNumber,
                startTransaction: true,
                autocommit: false,
                stmtId: NumberInt(2),
            }));

            // Retry the write statement executed in the external session.
            assert.commandWorked(testDB.runCommand({
                findAndModify: collName,
                query: {x: 0},
                update: {$inc: {y: 1}},
                new: true,
                lsid: childLsid,
                txnNumber: childTxnNumber,
                autocommit: false,
                stmtId: NumberInt(0),
            }));

            // Retry the write statement executed in the committed internal transaction.
            assert.commandWorked(testDB.runCommand({
                insert: collName,
                documents: [{x: 1}],
                lsid: childLsid,
                txnNumber: childTxnNumber,
                autocommit: false,
                stmtId: NumberInt(1),
            }));

            assert.commandWorked(
                primary.adminCommand(makeCommitTransactionCmdObj(childLsid, childTxnNumber)));
        };

    // Start another internal transaction in a separate thread, and make it hang right after it
    // finishes executing the first statement.
    const fp = configureFailPoint(primary, "hangInsertBeforeWrite", {ns: kNs});
    const internalTxnThread = new Thread(runInternalTxn,
                                         primary.host,
                                         extractUUIDFromObject(sessionOpts.sessionUUID),
                                         sessionOpts.parentTxnNumber.valueOf(),
                                         kDbName,
                                         kCollName);
    internalTxnThread.start();
    fp.wait();

    // Force the logical session cache to reap, and verify that the config.transactions and
    // config.image_collection entry for the retryable write in the external session and for the
    // committed internal transaction for that retryable write do not get reaped since there is an
    // in-progress internal transaction for the same retryable write.
    assert.commandWorked(sessionsColl.remove({"_id.id": sessionOpts.sessionUUID}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 2, numImageCollEntries: 1});

    fp.off();
    internalTxnThread.join();

    // Verify that the retried write statements did not re-execute.
    assert.eq(testColl.find({x: 0, y: 1}).itcount(), 1);
    assert.eq(testColl.find({x: 1}).itcount(), 1);
    assert.eq(testColl.find({x: 2}).itcount(), 1);

    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    assert.commandWorked(testColl.remove({}));
}

// Test reaping when an internal session is about to be checked out.

{
    jsTest.log("Test reaping when a retryable-write internal session is about to be checked out");
    const sessionOpts = makeSessionOptsForTest();

    assert.commandWorked(testColl.insert([{x: 0, y: 0}]));
    assert.commandWorked(testDB.runCommand({
        findAndModify: kCollName,
        query: {x: 0},
        update: {$inc: {y: 1}},
        new: true,
        lsid: sessionOpts.parentLsid,
        txnNumber: sessionOpts.parentTxnNumber,
        stmtId: NumberInt(0),
    }));

    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 1, numTransactionsCollEntries: 1, numImageCollEntries: 1});

    const runInternalTxn =
        (primaryHost, parentLsidUUIDString, parentTxnNumber, dbName, collName) => {
            load("jstests/sharding/libs/sharded_transactions_helpers.js");

            const primary = new Mongo(primaryHost);
            const testDB = primary.getDB(dbName);

            const childLsid = {
                id: UUID(parentLsidUUIDString),
                txnNumber: NumberLong(parentTxnNumber),
                txnUUID: UUID()
            };
            const childTxnNumber = NumberLong(0);

            // Retry the statement executed in the external session.
            assert.commandWorked(testDB.runCommand({
                insert: collName,
                documents: [{y: 0}],
                lsid: childLsid,
                txnNumber: childTxnNumber,
                startTransaction: true,
                autocommit: false,
                stmtId: NumberInt(0),
            }));
            assert.commandWorked(
                testDB.adminCommand(makeCommitTransactionCmdObj(childLsid, childTxnNumber)));
        };

    const fp = configureFailPoint(primary, "hangBeforeSessionCheckOut");
    const internalTxnThread = new Thread(runInternalTxn,
                                         primary.host,
                                         extractUUIDFromObject(sessionOpts.sessionUUID),
                                         sessionOpts.parentTxnNumber.valueOf(),
                                         kDbName,
                                         kCollName);
    internalTxnThread.start();
    fp.wait();

    // Force the logical session cache to reap, and verify that the config.transactions entry and
    // config.image_collection entry for the retryable write in the external session do get reaped.
    assert.commandWorked(sessionsColl.remove({"_id.id": sessionOpts.sessionUUID}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    // Verify that the internal transaction did not get interrupted but that the retried write
    // statement re-execute, i.e. retryablity is violated because the retry occurs after the session
    // got reaped.
    fp.off();
    internalTxnThread.join();

    assert.eq(testColl.find({x: 0, y: 1}).itcount(), 1);

    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
    assertNumEntries(
        sessionOpts,
        {numSessionsCollEntries: 0, numTransactionsCollEntries: 0, numImageCollEntries: 0});

    assert.commandWorked(testColl.remove({}));
}

rst.stopSet();
})();
