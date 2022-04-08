/**
 * Verify that during FCV downgrade we abort unprepared internal
 * transactions and wait for prepared internal transactions to
 * commit or abort.
 *
 * This test verifies this behavior with internal transactions in sessions with
 * retryableWrite: {false, true}, distinguished through the specification of txnNumber in their
 * lsids. Variables named with 0 correlates to the former and 1 with the latter.
 * @tags: [requires_fcv_60]
 */
(function() {
'use strict';

load("jstests/sharding/libs/sharded_transactions_helpers.js");
load("jstests/libs/fail_point_util.js");

const kDbName = "testDb";
const kCollName = "testColl";
let stmtId = 0;

function makeInsertCmdObj(childLsid, txnNumber, startTransaction) {
    const cmdObj = {
        insert: kCollName,
        documents: [{x: 0}],
        lsid: childLsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(stmtId++),
        autocommit: false,
    };
    if (startTransaction) {
        cmdObj.startTransaction = true;
    }

    return cmdObj;
}

(() => {
    jsTest.log(
        "Verify internal transactions in an in-progress, unprepared state are aborted when FCV is downgraded.");

    const st = new ShardingTest({shards: {rs0: {nodes: 2}}});
    const shard0Rst = st.rs0;
    const shard0Primary = shard0Rst.getPrimary();

    const testDB = shard0Primary.getDB(kDbName);

    const sessionUUID = UUID();
    const childLsid0 = {id: sessionUUID, txnUUID: UUID()};
    const childLsid1 = {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()};

    // Start internal transactions inside sessions with retryableWrite: {false, true}.
    assert.commandWorked(testDB.runCommand(makeInsertCmdObj(childLsid0, NumberLong(0), true)));
    assert.commandWorked(testDB.runCommand(makeInsertCmdObj(childLsid1, NumberLong(0), true)));

    // By being unable to insert documents, verify that both transactions are aborted when FCV is
    // downgraded.
    assert.commandWorked(shard0Primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    assert.commandFailedWithCode(testDB.runCommand(makeInsertCmdObj(childLsid0, NumberLong(0))),
                                 ErrorCodes.NoSuchTransaction);
    assert.commandFailedWithCode(testDB.runCommand(makeInsertCmdObj(childLsid1, NumberLong(0))),
                                 ErrorCodes.NoSuchTransaction);

    st.stop();
})();

(() => {
    jsTest.log(
        "Verify FCV cannot be downgraded when a session has a prepared internal transaction until the transaction is out of the prepared state (either aborted or committed).");

    const st = new ShardingTest({shards: 1});
    const shard0Rst = st.rs0;
    const shard0Primary = shard0Rst.getPrimary();

    const testDB = shard0Primary.getDB(kDbName);
    const adminDB = shard0Primary.getDB("admin");

    const sessionUUID = UUID();
    const childLsid0 = {id: sessionUUID, txnUUID: UUID()};
    const childLsid1 = {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()};

    assert.commandWorked(testDB.createCollection(kCollName));

    // Start internal transactions for writes executed in sessions with retryableWrite: {false,
    // true} and bring to prepared state.
    assert.commandWorked(testDB.runCommand(makeInsertCmdObj(childLsid0, NumberLong(0), true)));
    assert.commandWorked(testDB.runCommand(makeInsertCmdObj(childLsid1, NumberLong(0), true)));

    assert.commandWorked(
        shard0Primary.adminCommand(makePrepareTransactionCmdObj(childLsid0, NumberLong(0))));
    assert.commandWorked(
        shard0Primary.adminCommand(makePrepareTransactionCmdObj(childLsid1, NumberLong(0))));

    // The setFCV command will need to acquire a global S lock to complete. The global
    // lock is currently held by prepare, so that will block. We use a failpoint to make that
    // command fails when it tries to get the lock.
    let fp = configureFailPoint(shard0Primary, "failNonIntentLocksIfWaitNeeded");
    assert.commandFailedWithCode(testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                 ErrorCodes.LockTimeout);
    fp.wait();
    fp.off();

    assert.commandWorked(adminDB.runCommand(makeAbortTransactionCmdObj(childLsid0, NumberLong(0))));
    fp = configureFailPoint(shard0Primary, "failNonIntentLocksIfWaitNeeded");
    assert.commandFailedWithCode(testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                 ErrorCodes.LockTimeout);
    fp.wait();
    fp.off();
    assert.commandWorked(adminDB.runCommand(makeAbortTransactionCmdObj(childLsid1, NumberLong(0))));

    // We are able to downgrade FCV only when both transactions are no longer in the prepared state.
    assert.commandWorked(shard0Primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    st.stop();
})();
})();
