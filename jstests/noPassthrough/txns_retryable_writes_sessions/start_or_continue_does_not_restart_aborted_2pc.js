/**
 * A 2PC multi-shard transaction must not silently lose a write when a sub-router re-targets an
 * aborted participant with startOrContinueTransaction.
 *
 *   1. local_coll:   unsharded, on shard0.   foreign_coll: sharded, all chunks on shard1.
 *   2. Start a transaction; insert X into local (shard0 participant) and Y into foreign (shard1
 *      participant).
 *   3. shard1 has a low transactionLifetimeLimitSeconds, so its reaper aborts shard1's participant
 *      (-> kAbortedWithoutPrepare on the SAME primary) while shard0's participant stays alive. Y's
 *      pending write is discarded.
 *   4. Run an aggregate on local with a $lookup into foreign. shard0 sub-routes the $lookup to
 *      shard1 with startOrContinueTransaction:true.
 *
 * @tags: [
 *   requires_sharding,
 *   uses_transactions,
 *   uses_multi_shard_transaction,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const localColl = "local_coll";
const foreignColl = "foreign_coll";
const foreignNs = dbName + "." + foreignColl;

const st = new ShardingTest({shards: 2});

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs, key: {_id: 1}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: foreignNs, find: {_id: MinKey}, to: st.shard1.shardName}),
);
st.refreshCatalogCacheForNs(st.s, foreignNs);
if (!FeatureFlagUtil.isPresentAndEnabled(st.rs0.getPrimary(), "AuthoritativeShardsCRUD")) {
    assert.commandWorked(
        st.rs0.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: foreignNs}),
    );
    assert.commandWorked(
        st.rs1.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: foreignNs}),
    );
}

// Pre-populate local (so the $lookup has input) and pre-warm shard0's catalog cache for foreign so
// the in-txn $lookup doesn't trigger a refresh that snapshot read concern would reject.
assert.commandWorked(
    st.s.getDB(dbName)[localColl].insert({_id: 1, key: "Y"}, {writeConcern: {w: "majority"}}),
);
st.s
    .getDB(dbName)
    [localColl].aggregate([
        {$lookup: {from: foreignColl, localField: "key", foreignField: "_id", as: "joined"}},
    ])
    .toArray();

// shard1 reaper period ~= (tll * 500ms) clamped to [1s, 60s]; tll=1 -> ~1s. shard0 stays default,
// so only shard1's participant expires.
const shard1Primary = st.rs1.getPrimary();
assert.commandWorked(
    shard1Primary.adminCommand({setParameter: 1, transactionLifetimeLimitSeconds: 1}),
);

const lsid = {id: UUID()};
const txnNumber = NumberLong(1);
const mongosDB = st.s.getDB(dbName);
const abortedBaseline = shard1Primary.adminCommand({serverStatus: 1}).transactions.totalAborted;

assert.commandWorked(
    mongosDB.runCommand({
        insert: localColl,
        documents: [{_id: "X", key: "Y"}],
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);
assert.commandWorked(
    mongosDB.runCommand({
        insert: foreignColl,
        documents: [{_id: "Y", value: "should-be-lost"}],
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(1),
        autocommit: false,
    }),
);

// Wait for shard1's reaper to abort the expired participant (-> kAbortedWithoutPrepare).
assert.soon(
    () => shard1Primary.adminCommand({serverStatus: 1}).transactions.totalAborted > abortedBaseline,
    "shard1's transactionLifetimeLimitSeconds reaper should have aborted the participant",
    30000,
    500,
);

// Sub-router path: aggregate on local with $lookup into foreign -> shard0 sub-routes to shard1 with
// startOrContinueTransaction:true. shard1's participant is kAbortedWithoutPrepare, so the gate must
// refuse the restart instead of silently resurrecting it.
const aggRes = mongosDB.runCommand({
    aggregate: localColl,
    pipeline: [
        {$lookup: {from: foreignColl, localField: "key", foreignField: "_id", as: "joined"}},
    ],
    cursor: {},
    lsid: lsid,
    txnNumber: txnNumber,
    stmtId: NumberInt(2),
    autocommit: false,
});
assert.commandFailedWithCode(
    aggRes,
    ErrorCodes.NoSuchTransaction,
    "sub-router startOrContinueTransaction must be refused once shard1's participant was aborted",
);
assert(
    aggRes.errorLabels && aggRes.errorLabels.includes("TransientTransactionError"),
    "refusal must carry TransientTransactionError so the whole transaction is retried",
    {aggRes},
);

// The transaction is now aborted; commit must fail (no partial commit).
assert.commandFailedWithCode(
    st.s.adminCommand({commitTransaction: 1, lsid: lsid, txnNumber: txnNumber, autocommit: false}),
    ErrorCodes.NoSuchTransaction,
    "commit must fail because the transaction was aborted",
);

// No silent data loss: neither write persisted. (local still has only the pre-populated {_id: 1}.)
const localIds = st.s
    .getDB(dbName)
    [localColl].find({}, {_id: 1})
    .sort({_id: 1})
    .toArray()
    .map((d) => d._id);
const foreignIds = st.s
    .getDB(dbName)
    [foreignColl].find({}, {_id: 1})
    .sort({_id: 1})
    .toArray()
    .map((d) => d._id);
assert(!localIds.includes("X"), "X must NOT be committed (the whole transaction aborted)", {
    localIds,
});
assert(!foreignIds.includes("Y"), "Y must NOT be committed", {foreignIds});

st.stop();
