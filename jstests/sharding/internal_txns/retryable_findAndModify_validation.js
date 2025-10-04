/*
 * Tests that it is invalid to commit a retryable internal transaction with multiple findAndModify
 * pre or post images.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
import {withRetryOnTransientTxnErrorIncrementTxnNum} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {makeCommitTransactionCmdObj} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const st = new ShardingTest({shards: 1, rs: {nodes: 2, oplogSize: 1024}});

const kSize10MB = 10 * 1024 * 1024;

const kDbName = "testDb";
const kCollName = "testColl";
const mongosTestDB = st.s.getDB(kDbName);
const mongosTestColl = mongosTestDB.getCollection(kCollName);

assert.commandWorked(
    mongosTestColl.insert([
        {_id: -1, x: -1},
        {_id: -2, x: -2},
    ]),
);

const lsid = {
    id: UUID(),
    txnNumber: NumberLong(35),
    txnUUID: UUID(),
};

{
    jsTest.log("Test small transaction with multiple findAndModify pre/post images");
    const txnNumber = NumberLong(0);
    let stmtId = 0;
    withRetryOnTransientTxnErrorIncrementTxnNum(txnNumber, (txnNum) => {
        // findAndModify with pre-image.
        mongosTestDB.runCommand({
            findAndModify: kCollName,
            query: {_id: -1, x: -1},
            update: {$inc: {x: -10}},
            lsid: lsid,
            txnNumber: NumberLong(txnNum),
            stmtId: NumberInt(stmtId++),
            startTransaction: true,
            autocommit: false,
        });
        // findAndModify with post-image.
        mongosTestDB.runCommand({
            findAndModify: kCollName,
            query: {_id: -2, x: -2},
            update: {$inc: {x: -10}},
            new: true,
            lsid: lsid,
            txnNumber: NumberLong(txnNum),
            stmtId: NumberInt(stmtId++),
            autocommit: false,
        });
        assert.commandFailedWithCode(mongosTestDB.adminCommand(makeCommitTransactionCmdObj(lsid, txnNum)), 6054001);
    });
}

{
    jsTest.log("Test large transaction with multiple findAndModify pre/post images");
    const txnNumber = NumberLong(1);
    let stmtId = 0;
    withRetryOnTransientTxnErrorIncrementTxnNum(txnNumber, (txnNum) => {
        let makeInsertCmdObj = (doc) => {
            return {
                insert: kCollName,
                documents: [Object.assign(doc, {y: "a".repeat(kSize10MB)})],
                lsid: lsid,
                txnNumber: NumberLong(txnNum),
                stmtId: NumberInt(stmtId++),
                autocommit: false,
            };
        };

        mongosTestDB.runCommand(Object.assign(makeInsertCmdObj({_id: -100, x: 100}), {startTransaction: true}));
        // findAndModify with pre-image.
        mongosTestDB.runCommand({
            findAndModify: kCollName,
            query: {_id: -1, x: -1},
            update: {$inc: {x: -10}},
            lsid: lsid,
            txnNumber: NumberLong(txnNum),
            stmtId: NumberInt(stmtId++),
            autocommit: false,
        });
        mongosTestDB.runCommand(makeInsertCmdObj({_id: -200, x: -200}));
        // findAndModify with post-image.
        mongosTestDB.runCommand({
            findAndModify: kCollName,
            query: {_id: -2, x: -2},
            update: {$inc: {x: -10}},
            new: true,
            lsid: lsid,
            txnNumber: NumberLong(txnNum),
            stmtId: NumberInt(stmtId++),
            autocommit: false,
        });
        assert.commandFailedWithCode(mongosTestDB.adminCommand(makeCommitTransactionCmdObj(lsid, txnNum)), 6054002);
    });
}

st.stop();
