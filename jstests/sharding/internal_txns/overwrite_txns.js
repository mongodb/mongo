/*
 * Tests when internal transactions overwrite existing transactions.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});

const kDbName = "testDb";
const kCollName = "testColl";
const testDB = st.rs0.getPrimary().getDB(kDbName);
assert.commandWorked(testDB[kCollName].insert({x: 1}));  // Set up the collection.

(() => {
    jsTest.log("Verify in progress child transactions are aborted by higher txnNumbers");

    let clientTxnNumber = 5;
    const clientSession = {id: UUID()};
    const retryableChildSession = {
        id: clientSession.id,
        txnUUID: UUID(),
        txnNumber: NumberLong(clientTxnNumber)
    };
    const nonRetryableChildSession = {id: clientSession.id, txnUUID: UUID()};

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: clientSession,
        txnNumber: NumberLong(clientTxnNumber),
        startTransaction: true,
        autocommit: false
    }));

    // A new child transaction should abort an existing client transaction.
    clientTxnNumber++;
    retryableChildSession.txnNumber = NumberLong(clientTxnNumber);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: retryableChildSession,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));
    // The client transaction should have been aborted.
    assert.commandFailedWithCode(testDB.adminCommand({
        commitTransaction: 1,
        lsid: clientSession,
        txnNumber: NumberLong(clientTxnNumber - 1),
        autocommit: false
    }),
                                 ErrorCodes.TransactionTooOld);

    // A non-retryable child transaction shouldn't affect retryable operations.
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: nonRetryableChildSession,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));
    // The retryable child transaction should still be open.
    assert.commandWorked(testDB.runCommand({
        find: kCollName,
        lsid: retryableChildSession,
        txnNumber: NumberLong(0),
        autocommit: false
    }));

    // A new child transaction should abort a lower child transaction.
    clientTxnNumber++;
    let retryableChildSessionCopy = Object.merge({}, retryableChildSession);
    retryableChildSession.txnNumber = NumberLong(clientTxnNumber);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: retryableChildSession,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));
    // The child transaction should have been aborted.
    assert.commandFailedWithCode(testDB.adminCommand({
        commitTransaction: 1,
        lsid: retryableChildSessionCopy,
        txnNumber: NumberLong(0),
        autocommit: false
    }),
                                 ErrorCodes.TransactionTooOld);

    // A new client transaction should abort a lower child transaction.
    clientTxnNumber++;
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: clientSession,
        txnNumber: NumberLong(clientTxnNumber),
        startTransaction: true,
        autocommit: false
    }));
    // The client transaction should have been aborted.
    assert.commandFailedWithCode(testDB.adminCommand({
        commitTransaction: 1,
        lsid: retryableChildSessionCopy,
        txnNumber: NumberLong(0),
        autocommit: false
    }),
                                 ErrorCodes.TransactionTooOld);

    // A new retryable write should abort a lower child transaction.
    clientTxnNumber++;
    retryableChildSession.txnNumber = NumberLong(clientTxnNumber);
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: retryableChildSession,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));
    clientTxnNumber++;
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: clientSession,
        txnNumber: NumberLong(clientTxnNumber)
    }));
    // The child transaction should have been aborted.
    assert.commandFailedWithCode(testDB.adminCommand({
        commitTransaction: 1,
        lsid: retryableChildSession,
        txnNumber: NumberLong(0),
        autocommit: false
    }),
                                 ErrorCodes.TransactionTooOld);

    // The non-retryable child transaction should still be open.
    assert.commandWorked(testDB.adminCommand({
        commitTransaction: 1,
        lsid: nonRetryableChildSession,
        txnNumber: NumberLong(0),
        autocommit: false
    }));
})();

(() => {
    jsTest.log("Verify prepared child transactions are not aborted by higher txnNumbers");

    let clientTxnNumber = 5;
    const clientSession = {id: UUID()};
    const retryableChildSession = {
        id: clientSession.id,
        txnUUID: UUID(),
        txnNumber: NumberLong(clientTxnNumber)
    };
    const nonRetryableChildSession = {id: clientSession.id, txnUUID: UUID()};

    // Prepare a retryable and non-retryable child transaction.

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: nonRetryableChildSession,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(testDB.adminCommand({
        prepareTransaction: 1,
        lsid: nonRetryableChildSession,
        txnNumber: NumberLong(0),
        autocommit: false
    }));

    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: retryableChildSession,
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(testDB.adminCommand({
        prepareTransaction: 1,
        lsid: retryableChildSession,
        txnNumber: NumberLong(0),
        autocommit: false
    }));

    // Verify a higher txnNumber cannot be accepted until the retryable transaction exits prepare.
    // Test all three sources of a higher txnNumber: client retryable write, client transaction, and
    // a retryable child session transaction.
    clientTxnNumber++;
    assert.commandFailedWithCode(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: clientSession,
        txnNumber: NumberLong(clientTxnNumber),
        maxTimeMS: 1000
    }),
                                 ErrorCodes.MaxTimeMSExpired);

    clientTxnNumber++;
    assert.commandFailedWithCode(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: clientSession,
        txnNumber: NumberLong(clientTxnNumber),
        startTransaction: true,
        autocommit: false,
        maxTimeMS: 1000
    }),
                                 ErrorCodes.MaxTimeMSExpired);

    clientTxnNumber++;
    assert.commandFailedWithCode(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: {id: clientSession.id, txnUUID: UUID(), txnNumber: NumberLong(clientTxnNumber)},
        txnNumber: NumberLong(clientTxnNumber),
        startTransaction: true,
        autocommit: false,
        maxTimeMS: 1000
    }),
                                 ErrorCodes.MaxTimeMSExpired);

    // Verify a transaction blocked on a prepared child transaction can become unstuck and succeed
    // once the child transaction exits prepare.
    const fp = configureFailPoint(
        st.rs0.getPrimary(),
        "waitAfterNewStatementBlocksBehindOpenInternalTransactionForRetryableWrite");
    const newTxnThread = new Thread((host, lsidUUID, txnNumber) => {
        const lsid = {id: UUID(lsidUUID)};

        const conn = new Mongo(host);
        assert.commandWorked(conn.getDB("foo").runCommand({
            insert: "test",
            documents: [{x: 1}],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            startTransaction: true,
            autocommit: false,
        }));
        assert.commandWorked(conn.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false
        }));
    }, st.s.host, extractUUIDFromObject(clientSession.id), clientTxnNumber);
    newTxnThread.start();

    // Wait for the side transaction to hit a PreparedTransactionInProgress error, then resolve the
    // prepared transaction and verify the side transaction can successfully complete.
    fp.wait();
    fp.off();

    assert.commandWorked(testDB.adminCommand({
        abortTransaction: 1,
        lsid: retryableChildSession,
        txnNumber: NumberLong(0),
        autocommit: false
    }));

    newTxnThread.join();

    // A higher txnNumber is accepted despite the prepared non-retryable child transaction.
    clientTxnNumber++;
    assert.commandWorked(testDB.runCommand({
        insert: kCollName,
        documents: [{x: 1}],
        lsid: clientSession,
        txnNumber: NumberLong(clientTxnNumber)
    }));

    assert.commandWorked(testDB.adminCommand({
        abortTransaction: 1,
        lsid: nonRetryableChildSession,
        txnNumber: NumberLong(0),
        autocommit: false
    }));
})();

st.stop();
})();
