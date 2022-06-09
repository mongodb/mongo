/*
 * Test that a retryable write or retryable internal transaction that is initiated while the
 * session has an open retryable internal transaction (i.e. one that has not committed or aborted)
 * is blocked until the transaction commits or aborts and does not cause any write statements that
 * have already executed to execute again.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/rslib.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
let shard0Primary = st.rs0.getPrimary();

const kDbName = "testDb";
const kCollName = "testColl";
const mongosTestDB = st.s.getDB(kDbName);
const mongosTestColl = mongosTestDB.getCollection(kCollName);
let shard0TestDB = st.rs0.getPrimary().getDB(kDbName);
let shard0TestColl = shard0TestDB.getCollection(kCollName);

assert.commandWorked(shard0TestDB.createCollection(kCollName));

function stepDownShard0Primary() {
    const oldPrimary = st.rs0.getPrimary();
    const oldSecondary = st.rs0.getSecondary();
    assert.commandWorked(oldSecondary.adminCommand({replSetFreeze: 0}));
    assert.commandWorked(
        oldPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    const newPrimary = st.rs0.getPrimary();
    assert.neq(oldPrimary, newPrimary);
    shard0Primary = newPrimary;
    shard0TestDB = shard0Primary.getDB(kDbName);
    shard0TestColl = shard0TestDB.getCollection(kCollName);
}

const parentLsid = {
    id: UUID()
};
let currentParentTxnNumber = 35;

/*
 * Runs a write statement inside a retryable internal transaction and then runs 'retryFunc' to
 * resend the write statement while the transaction is still open. Verifies that the retry is
 * blocked until the transaction commits or aborts and does not cause the write statement to execute
 * more than once.
 */
function testBlockingRetry(retryFunc, testOpts = {
    prepareBeforeRetry,
    abortAfterBlockingRetry,
    stepDownPrimaryAfterBlockingRetry
}) {
    jsTest.log("Test blocking retry with test options " + tojson(testOpts));
    const parentTxnNumber = currentParentTxnNumber++;
    const docToInsert = {x: 1};
    const stmtId = 1;

    // Start a retryable internal transaction.
    const childLsid = {id: parentLsid.id, txnNumber: NumberLong(parentTxnNumber), txnUUID: UUID()};
    const childTxnNumber = NumberLong(0);

    const originalWriteCmdObj = {
        insert: kCollName,
        documents: [docToInsert],
        lsid: childLsid,
        txnNumber: NumberLong(childTxnNumber),
        startTransaction: true,
        autocommit: false,
        stmtId: NumberInt(stmtId),
    };
    const prepareCmdObj = makePrepareTransactionCmdObj(childLsid, childTxnNumber);
    const commitCmdObj = makeCommitTransactionCmdObj(childLsid, childTxnNumber);
    const abortCmdObj = makeAbortTransactionCmdObj(childLsid, childTxnNumber);

    assert.commandWorked(shard0TestDB.runCommand(originalWriteCmdObj));
    if (testOpts.prepareBeforeRetry) {
        const preparedTxnRes = assert.commandWorked(shard0Primary.adminCommand(prepareCmdObj));
        commitCmdObj.commitTimestamp = preparedTxnRes.prepareTimestamp;
    }

    let fp;
    if (testOpts.prepareBeforeRetry) {
        // A prepared transaction cannot be interrupted by a retry so retry and wait for it to block
        // behind the internal transaction above.
        fp = configureFailPoint(
            shard0Primary,
            "waitAfterNewStatementBlocksBehindOpenInternalTransactionForRetryableWrite");
    }
    const retryThread = new Thread(retryFunc, {
        shard0RstArgs: createRstArgs(st.rs0),
        parentSessionUUIDString: extractUUIDFromObject(parentLsid.id),
        parentTxnNumber,
        docToInsert,
        stmtId,
        dbName: kDbName,
        collName: kCollName,
        stepDownPrimaryAfterBlockingRetry: testOpts.stepDownPrimaryAfterBlockingRetry
    });
    retryThread.start();
    if (testOpts.prepareBeforeRetry) {
        // The retry should block behind the prepared transaction.
        fp.wait();
        fp.off();
    } else {
        // The retry should complete without blocking.
        retryThread.join();
    }

    if (testOpts.stepDownPrimaryAfterBlockingRetry) {
        stepDownShard0Primary();
    }

    // Commit or abort the internal transaction, and verify that the write statement executed
    // exactly once despite the concurrent retry, whether or not the retry interrupted the original
    // attempt.
    if (testOpts.prepareBeforeRetry) {
        assert.commandWorked(shard0TestDB.adminCommand(
            testOpts.abortAfterBlockingRetry ? abortCmdObj : commitCmdObj));
        retryThread.join();
    } else {
        // The retry should have interrupted the original attempt.
        assert.commandFailedWithCode(
            shard0TestDB.adminCommand(testOpts.abortAfterBlockingRetry ? abortCmdObj
                                                                       : commitCmdObj),
            ErrorCodes.NoSuchTransaction);
    }
    assert.eq(shard0TestColl.count(docToInsert), 1);

    assert.commandWorked(mongosTestColl.remove({}));
}

function runTests(retryFunc) {
    testBlockingRetry(retryFunc, {
        prepareBeforeRetry: false,
        abortAfterBlockingRetry: false,
        stepDownPrimaryAfterBlockingRetry: false
    });
    testBlockingRetry(retryFunc, {
        prepareBeforeRetry: false,
        abortAfterBlockingRetry: true,
        stepDownPrimaryAfterBlockingRetry: false
    });
    testBlockingRetry(retryFunc, {
        prepareBeforeRetry: true,
        abortAfterBlockingRetry: false,
        stepDownPrimaryAfterBlockingRetry: false
    });
    testBlockingRetry(retryFunc, {
        prepareBeforeRetry: true,
        abortAfterBlockingRetry: false,
        stepDownPrimaryAfterBlockingRetry: true
    });
    testBlockingRetry(retryFunc, {
        prepareBeforeRetry: true,
        abortAfterBlockingRetry: true,
        stepDownPrimaryAfterBlockingRetry: false
    });
    testBlockingRetry(retryFunc, {
        prepareBeforeRetry: true,
        abortAfterBlockingRetry: true,
        stepDownPrimaryAfterBlockingRetry: true
    });
}

{
    jsTest.log(
        "Test retrying write statement executed in a retryable internal transaction as a " +
        "retryable write in the parent session while the transaction has not been committed " +
        "or aborted");

    let retryFunc = (testOpts) => {
        load("jstests/replsets/rslib.js");
        load("jstests/sharding/libs/sharded_transactions_helpers.js");
        const shard0Rst = createRst(testOpts.shard0RstArgs);
        let shard0Primary = shard0Rst.getPrimary();

        const retryWriteCmdObj = {
            insert: testOpts.collName,
            documents: [testOpts.docToInsert],
            lsid: {id: UUID(testOpts.parentSessionUUIDString)},
            txnNumber: NumberLong(testOpts.parentTxnNumber),
            stmtId: NumberInt(testOpts.stmtId)
        };

        let retryRes = shard0Primary.getDB(testOpts.dbName).runCommand(retryWriteCmdObj);
        if (testOpts.stepDownPrimaryAfterBlockingRetry) {
            assert(ErrorCodes.isNotPrimaryError(retryRes.code));
            shard0Primary = shard0Rst.getPrimary();
            retryRes = shard0Primary.getDB(testOpts.dbName).runCommand(retryWriteCmdObj);
        }
        assert.commandWorked(retryRes);
    };

    runTests(retryFunc);
}

{
    jsTest.log(
        "Test retrying write statement executed in a retryable internal transaction in a " +
        "different retryable internal transaction while the original transaction has not been " +
        "committed or aborted");

    let retryFunc = (testOpts) => {
        load("jstests/replsets/rslib.js");
        load("jstests/sharding/libs/sharded_transactions_helpers.js");
        const shard0Rst = createRst(testOpts.shard0RstArgs);
        let shard0Primary = shard0Rst.getPrimary();

        const retryChildLsid = {
            id: UUID(testOpts.parentSessionUUIDString),
            txnNumber: NumberLong(testOpts.parentTxnNumber),
            txnUUID: UUID()
        };
        const retryChildTxnNumber = NumberLong(0);
        const retryWriteCmdObj = {
            insert: testOpts.collName,
            documents: [testOpts.docToInsert],
            lsid: retryChildLsid,
            txnNumber: NumberLong(retryChildTxnNumber),
            startTransaction: true,
            autocommit: false,
            stmtId: NumberInt(testOpts.stmtId),
        };
        const retryCommitCmdObj = makeCommitTransactionCmdObj(retryChildLsid, retryChildTxnNumber);

        let retryRes = shard0Primary.getDB(testOpts.dbName).runCommand(retryWriteCmdObj);
        if (testOpts.stepDownPrimaryAfterBlockingRetry) {
            assert(ErrorCodes.isNotPrimaryError(retryRes.code));
            shard0Primary = shard0Rst.getPrimary();
            retryRes = shard0Primary.getDB(testOpts.dbName).runCommand(retryWriteCmdObj);
        }
        assert.commandWorked(retryRes);
        assert.commandWorked(shard0Primary.getDB(testOpts.dbName).adminCommand(retryCommitCmdObj));
    };

    runTests(retryFunc);
}

st.stop();
})();
