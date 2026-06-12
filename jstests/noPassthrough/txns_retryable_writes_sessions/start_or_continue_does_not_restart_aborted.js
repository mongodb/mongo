/**
 * 'startOrContinueTransaction' request must not restart a transaction participant in place from an
 * aborted state.
 *
 * @tags: [
 *   requires_sharding,
 *   uses_transactions,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = "start_or_continue_does_not_restart_aborted";

const st = new ShardingTest({shards: 1});
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(st.s.getDB(dbName).createCollection(collName));

const shard0Primary = st.rs0.getPrimary();
const shardDB = shard0Primary.getDB(dbName);
const lsid = {id: UUID()};
const txnNumber = NumberLong(1);

jsTest.log.info("Start a transaction, insert X, read it back, then abort it.");
assert.commandWorked(
    shardDB.runCommand({
        insert: collName,
        documents: [{_id: "X"}],
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);
const inTxnRead = assert.commandWorked(
    shardDB.runCommand({
        find: collName,
        filter: {_id: "X"},
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(1),
        autocommit: false,
    }),
);
assert.eq(1, inTxnRead.cursor.firstBatch.length, "X should be visible inside the txn");

assert.commandWorked(
    shard0Primary.adminCommand({
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(2),
        autocommit: false,
    }),
);

jsTest.log.info(
    "Reusing the aborted (lsid, txnNumber) via startOrContinueTransaction must be " +
        "rejected, not silently resurrected.",
);
const res = shardDB.runCommand({
    insert: collName,
    documents: [{_id: "Y"}],
    lsid: lsid,
    txnNumber: txnNumber,
    stmtId: NumberInt(0),
    startOrContinueTransaction: true,
    autocommit: false,
});
assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
assert(
    res.errorLabels && res.errorLabels.includes("TransientTransactionError"),
    "rejection should carry the TransientTransactionError label so the driver retries",
    {res},
);

jsTest.log.info(
    "No silent data loss: X was discarded by the abort and Y was rejected, so " +
        "neither persisted.",
);
const docs = st.s.getDB(dbName)[collName].find({}, {_id: 1}).sort({_id: 1}).toArray();
assert.eq([], docs, "collection should be empty", {docs});

st.stop();
