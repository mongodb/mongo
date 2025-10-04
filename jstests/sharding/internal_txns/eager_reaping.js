/*
 * Tests that transaction records for retryable internal sessions are reaped eagerly when they are
 * reaped early from memory.
 *
 * @tags: [
 *    requires_fcv_60,
 *    uses_transactions,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, config: 1});

const kDbName = "testDb";
const kCollName = "testColl";
const mongosTestColl = st.s.getCollection(kDbName + "." + kCollName);
// Reap all internal sessions to make sure the actual test case starts without anything added
// implicitly from setting up the test collection.
assert.commandWorked(st.rs0.getPrimary().adminCommand({setParameter: 1, internalSessionsReapThreshold: 1}));
assert.commandWorked(mongosTestColl.insert({x: 1})); // Set up the collection.

function assertNumEntries(conn, {sessionUUID, numImageCollectionEntries, numTransactionsCollEntries}) {
    const filter = {"_id.id": sessionUUID};

    const imageColl = conn.getCollection("config.image_collection");
    assert.eq(numImageCollectionEntries, imageColl.find(filter).itcount(), tojson(imageColl.find().toArray()));

    const transactionsColl = conn.getCollection("config.transactions");
    assert.eq(
        numTransactionsCollEntries,
        transactionsColl.find(filter).itcount(),
        tojson(transactionsColl.find().toArray()),
    );
}

function runInternalTxn(conn, lsid, txnNumber) {
    const testInternalTxnCmdObj = {
        testInternalTransactions: 1,
        commandInfos: [
            {
                dbName: kDbName,
                command: {
                    // Use findAndModify to generate image collection entries.
                    findAndModify: kCollName,
                    query: {x: 1},
                    update: {$inc: {counter: 1}},
                    stmtId: NumberInt(3),
                },
            },
        ],
        lsid: lsid,
    };
    if (txnNumber !== undefined) {
        testInternalTxnCmdObj.txnNumber = NumberLong(txnNumber);
    }
    assert.commandWorked(conn.adminCommand(testInternalTxnCmdObj));
}

function assertNumEntriesSoon(shardConn, {sessionUUID, numImageCollectionEntries, numTransactionsCollEntries}) {
    // Sleep a little so it's likely the reaping has finished and we can avoid spamming the logs.
    sleep(100);
    assert.soonNoExcept(
        () => {
            assertNumEntries(shardConn, {sessionUUID, numImageCollectionEntries, numTransactionsCollEntries});
            return true;
        },
        "Expected internal transactions to be reaped eventually",
        undefined,
        100 /* interval */,
    );
}

function runTest(conn, shardConn) {
    // Lower the threshold to speed up the test and verify it's respected.
    const reapThreshold = 100;
    assert.commandWorked(shardConn.adminCommand({setParameter: 1, internalSessionsReapThreshold: reapThreshold}));

    //
    // Reaping happens at the threshold.
    //

    let parentLsid = {id: UUID()};

    // No transaction records at first.
    assertNumEntries(shardConn, {
        sessionUUID: parentLsid.id,
        numImageCollectionEntries: 0,
        numTransactionsCollEntries: 0,
    });

    // Records build up until the reap threshold.
    for (let i = 0; i < reapThreshold; i++) {
        runInternalTxn(conn, parentLsid, i);
    }
    assertNumEntries(shardConn, {
        sessionUUID: parentLsid.id,
        numImageCollectionEntries: reapThreshold,
        numTransactionsCollEntries: reapThreshold,
    });

    // Push the number of eagerly reaped sessions up to the threshold and verify this triggers
    // reaping them.
    runInternalTxn(conn, parentLsid, reapThreshold + 1);
    assertNumEntriesSoon(shardConn, {
        sessionUUID: parentLsid.id,
        numImageCollectionEntries: 1,
        numTransactionsCollEntries: 1,
    });

    //
    // Reaping can run more than once.
    //

    for (let i = 0; i < reapThreshold; i++) {
        // We're on the same session as before, so pick higher txnNumbers than used before.
        const txnNumber = i + reapThreshold + 1;
        runInternalTxn(conn, parentLsid, txnNumber);
    }
    assertNumEntriesSoon(shardConn, {
        sessionUUID: parentLsid.id,
        numImageCollectionEntries: 1,
        numTransactionsCollEntries: 1,
    });

    //
    // Buffered sessions are cleared on failover.
    //

    parentLsid = {id: UUID()};

    const numBeforeFailover = reapThreshold / 2 + 1;
    for (let i = 0; i < numBeforeFailover; i++) {
        runInternalTxn(conn, parentLsid, i);
    }
    assertNumEntries(shardConn, {
        sessionUUID: parentLsid.id,
        numImageCollectionEntries: numBeforeFailover,
        numTransactionsCollEntries: numBeforeFailover,
    });

    // Step down and back up the new primary and verify it only reaps newly expired internal
    // sessions.

    assert.commandWorked(shardConn.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    assert.commandWorked(shardConn.adminCommand({replSetFreeze: 0}));
    st.rs0.stepUp(shardConn);
    shardConn = st.rs0.getPrimary();

    const numAfterFailover = reapThreshold / 2 + 1;
    assert(numAfterFailover + numBeforeFailover > reapThreshold);
    for (let i = 0; i < numAfterFailover; i++) {
        const txnNumber = i + numBeforeFailover; // Account for txnNumbers used before failover.
        runInternalTxn(conn, parentLsid, txnNumber);
    }
    assertNumEntries(shardConn, {
        sessionUUID: parentLsid.id,
        numImageCollectionEntries: numBeforeFailover + numAfterFailover,
        numTransactionsCollEntries: numBeforeFailover + numAfterFailover,
    });

    // Insert up to the threshold and verify a reap is triggered.
    for (let i = 0; i < reapThreshold - numAfterFailover; i++) {
        const txnNumber = i + 1000; // Account for txnNumbers used earlier.
        runInternalTxn(conn, parentLsid, txnNumber);
    }
    assertNumEntriesSoon(shardConn, {
        sessionUUID: parentLsid.id,
        numImageCollectionEntries: numBeforeFailover,
        numTransactionsCollEntries: numBeforeFailover,
    });

    //
    // Reaping ignores non-retryable sessions and parent sessions.
    //

    parentLsid = {id: UUID()};

    runInternalTxn(conn, parentLsid); // Non-retryable transaction.
    assert.commandWorked(
        conn.getDB(kDbName).runCommand({
            insert: "foo",
            documents: [{x: 1}],
            lsid: parentLsid,
            txnNumber: NumberLong(0),
            stmtId: NumberInt(0),
        }),
    );

    // Run enough retryable transactions to trigger a reap.
    for (let i = 0; i < reapThreshold + 1; i++) {
        const txnNumber = i + 1; // Account for the retryable write's txnNumber.
        runInternalTxn(conn, parentLsid, txnNumber);
    }
    // Expect 3: the parent entry, the non-retryable entry, and the latest retryable child. Only the
    // retryable child has an image entry, so just expect 1 of those.
    assertNumEntriesSoon(shardConn, {
        sessionUUID: parentLsid.id,
        numImageCollectionEntries: 1,
        numTransactionsCollEntries: 3,
    });
}

// Validates behavior about the configurable reap threshold server parameter.
function runParameterTest(conn, shardConn) {
    // Must be a number.
    assert.commandFailedWithCode(
        shardConn.adminCommand({setParameter: 1, internalSessionsReapThreshold: "wontwork"}),
        ErrorCodes.BadValue,
    );

    // Can't be set negative.
    assert.commandFailedWithCode(
        shardConn.adminCommand({setParameter: 1, internalSessionsReapThreshold: -1}),
        ErrorCodes.BadValue,
    );

    // Can be set to 0 or a positive value.
    assert.commandWorked(shardConn.adminCommand({setParameter: 1, internalSessionsReapThreshold: 0}));
    assert.commandWorked(shardConn.adminCommand({setParameter: 1, internalSessionsReapThreshold: 12345}));

    // Doesn't exist on mongos. This fails with no error code so check the errmsg.
    const res = assert.commandFailed(conn.adminCommand({setParameter: 1, internalSessionsReapThreshold: 222}));
    assert(res.errmsg.includes("unrecognized parameter"), tojson(res));
}

runTest(st.s, st.rs0.getPrimary());
runParameterTest(st.s, st.rs0.getPrimary());

st.stop();
