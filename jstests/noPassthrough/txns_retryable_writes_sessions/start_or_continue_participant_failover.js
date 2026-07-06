/**
 * Tests that a sharded multi-document transaction aborts with a retryable error, instead of
 * silently committing a partial result, when a participant's primary fails over mid-transaction.
 *
 * The router records each participant's replication term on first contact and validates it on
 * every subsequent response. A failover bumps the term, which the router detects when a
 * sub-router re-contacts the new primary via startOrContinueTransaction, aborting the
 * transaction with NoSuchTransaction (a TransientTransactionError) so the driver retries.
 *
 * @tags: [
 *   requires_sharding,
 *   uses_transactions,
 *   uses_multi_shard_transaction,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const localColl = "local_coll";
const foreignColl = "foreign_coll";
const foreignNs = dbName + "." + foreignColl;

// Raise the transaction lifetime limit so the periodic aborter can't abort the open txn on a
// slow host — that would yield the expected NoSuchTransaction for the wrong reason.
const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 3, setParameter: {transactionLifetimeLimitSeconds: 24 * 60 * 60}},
});

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

// Shard `foreign` and place all chunks on shard1.
assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs, key: {_id: 1}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: foreignNs, find: {_id: MinKey}, to: st.shard1.shardName}),
);
st.refreshCatalogCacheForNs(st.s, foreignNs);
assert.commandWorked(st.rs0.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: foreignNs}));
assert.commandWorked(st.rs1.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: foreignNs}));

// Pre-populate `local` (unsharded, on shard0) so the aggregate has input.
assert.commandWorked(
    st.s.getDB(dbName)[localColl].insert({_id: 1, key: "Y"}, {writeConcern: {w: "majority"}}),
);

// Pre-warm shard0's catalog cache for `foreign_coll`: inside a snapshot-read txn, mongos won't
// refresh stale catalog entries (it would break snapshot consistency).
st.s
    .getDB(dbName)
    [localColl].aggregate([
        {$lookup: {from: foreignColl, localField: "key", foreignField: "_id", as: "joined"}},
    ])
    .toArray();

const lsid = {id: UUID()};
const txnNumber = NumberLong(1);
const mongosDB = st.s.getDB(dbName);

jsTest.log("Step 1: insert {_id: 'X'} into local (shard0).");
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

jsTest.log("Step 2: insert {_id: 'Y'} into foreign (shard1).");
assert.commandWorked(
    mongosDB.runCommand({
        insert: foreignColl,
        documents: [{_id: "Y", value: "should-be-lost-without-fix"}],
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(1),
        autocommit: false,
    }),
);

jsTest.log(
    "Step 3: step up a different node on shard1; the in-progress participant and its pending write are discarded.",
);
const oldPrimary = st.rs1.getPrimary();
const electionTarget = st.rs1.getSecondaries()[0];
st.rs1.stepUp(electionTarget);
st.rs1.awaitNodesAgreeOnPrimary();
const newPrimary = st.rs1.getPrimary();
jsTest.log.info("shard1 primary changed", {
    oldPrimary: oldPrimary.host,
    newPrimary: newPrimary.host,
});
assert.neq(oldPrimary.host, newPrimary.host, "stepUp did not change the primary");

// Settle routing state on the new primary with a non-transactional read before re-entering the
// txn; otherwise the in-txn aggregate hits a retriable routing error from inside a snapshot-read.
st.s.getDB(dbName)[foreignColl].find().toArray();

jsTest.log(
    "Step 4: aggregate with $lookup: shard0 sub-routes to shard1's new primary, and the router detects the term bump.",
);
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

jsTest.log(
    "Step 5: the aggregate (or the eventual commit) must fail with NoSuchTransaction (TransientTransactionError).",
);

if (!aggRes.ok) {
    jsTest.log.info("aggregate failed as expected", {aggRes});
    assert.eq(
        aggRes.code,
        ErrorCodes.NoSuchTransaction,
        "expected NoSuchTransaction on participant failover detection",
        {aggRes},
    );
    assert(
        aggRes.errorLabels && aggRes.errorLabels.includes("TransientTransactionError"),
        "expected TransientTransactionError label so the driver retries",
        {aggRes},
    );
} else {
    // If the aggregate bypassed the validation path, the failure must surface at commit time
    // instead. Either way, a partial commit is unacceptable.
    const commitRes = st.s.adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
    });
    jsTest.log.info("commitTransaction returned", {commitRes});
    assert.commandFailedWithCode(
        commitRes,
        ErrorCodes.NoSuchTransaction,
        "expected NoSuchTransaction at commit if it wasn't already raised at the aggregate",
    );
}

jsTest.log("Step 6: verify no partial commit landed on disk.");
const localDocs = st.s.getDB(dbName)[localColl].find({}, {_id: 1}).sort({_id: 1}).toArray();
const foreignDocs = st.s.getDB(dbName)[foreignColl].find({}, {_id: 1}).sort({_id: 1}).toArray();
jsTest.log.info("collection contents after abort", {localDocs, foreignDocs});

const localIds = localDocs.map((d) => d._id);
const foreignIds = foreignDocs.map((d) => d._id);

assert(
    !localIds.includes("X"),
    "X should NOT be present in local on shard0 because the transaction aborted",
);
assert(
    !foreignIds.includes("Y"),
    "Y should NOT be present in foreign on shard1 because the transaction aborted",
);

jsTest.log(
    "Step 7: re-run the whole transaction (what a driver does on TransientTransactionError); both writes must now land.",
);
const retryTxnNumber = NumberLong(2);
assert.commandWorked(
    mongosDB.runCommand({
        insert: localColl,
        documents: [{_id: "X", key: "Y"}],
        lsid: lsid,
        txnNumber: retryTxnNumber,
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);
assert.commandWorked(
    mongosDB.runCommand({
        insert: foreignColl,
        documents: [{_id: "Y", value: "durable-after-retry"}],
        lsid: lsid,
        txnNumber: retryTxnNumber,
        stmtId: NumberInt(1),
        autocommit: false,
    }),
);
assert.commandWorked(
    mongosDB.runCommand({
        aggregate: localColl,
        pipeline: [
            {$lookup: {from: foreignColl, localField: "key", foreignField: "_id", as: "joined"}},
        ],
        cursor: {},
        lsid: lsid,
        txnNumber: retryTxnNumber,
        stmtId: NumberInt(2),
        autocommit: false,
    }),
);
assert.commandWorked(
    st.s.adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: retryTxnNumber,
        autocommit: false,
    }),
);

assert.eq(
    1,
    st.s.getDB(dbName)[localColl].find({_id: "X"}).itcount(),
    "X must be durable in local after the retried transaction committed",
);
assert.eq(
    1,
    st.s.getDB(dbName)[foreignColl].find({_id: "Y"}).itcount(),
    "Y must be durable in foreign after the retried transaction committed",
);

jsTest.log(
    "Confirmed: failover detected, transaction aborted retryably, no partial commit, and the retry succeeded.",
);

st.stop();
