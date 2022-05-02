/*
 * Tests that we send the proper error back to the client to retry the transaction if there's a
 * failover and FCV change that causes the server to lose previous transaction metadata.
 */

(function() {
'use strict';

load("jstests/libs/uuid_util.js");

const kDbName = "testDb";
const kCollName = "testColl";

const ns = kDbName + "." + kCollName;

// A monotonically increasing value to deduplicate document insertions.
let docVal = 5;

const st = new ShardingTest({shards: 2});

let coordinator = st.shard0;
let participant1 = st.shard1;

// Create a sharded collection with a chunk on each shard:
// shard0: [-inf, 0)
// shard1: [0, +inf)
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: kDbName, to: coordinator.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));

st.refreshCatalogCacheForNs(st.s, ns);

function runTest(lsid) {
    jsTest.log("Test that the correct error response is propagated upon losing in memory " +
               "transaction metadata and durable metadata in the config.transactions collection " +
               "after FCV change and failover with lsid: " + tojson(lsid));

    // Upgrade fcv to make sure cluster is on the latestFCV before starting any transactions.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Inserts are split to guarantee that shard0 will be chosen as the coordinator.
    assert.commandWorked(st.s.getDB(kDbName).runCommand({
        insert: kCollName,
        documents: [{_id: -1 * docVal}],
        lsid: lsid,
        txnNumber: NumberLong(0),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }));

    assert.commandWorked(st.s.getDB(kDbName).runCommand({
        insert: kCollName,
        documents: [{_id: docVal}],
        lsid: lsid,
        txnNumber: NumberLong(0),
        stmtId: NumberInt(1),
        autocommit: false,
    }));

    assert.commandWorked(st.s.adminCommand(
        {commitTransaction: 1, lsid: lsid, txnNumber: NumberLong(0), autocommit: false}));

    // Downgrading should remove config.transactions entries for internal transactions.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    // Restarting should clear out in memory transaction metadata.
    st.shard0.rs.stopSet(null /* signal */, true /* forRestart */);
    st.shard0.rs.startSet({restart: true});

    // For the following two statements, we are emulating the retryability behavior that mongos will
    // display. Without config.transactions metadata, we shouldn't be able to get information on
    // previous transactions so further commits should throw NoSuchTransaction. Since
    // NoSuchTransaction is a TransientTransactionError, we would retry the transaction, which
    // ultimately should fail with InternalTransactionNotSupported (since we've just downgraded),
    // which the client should deem as a retryable write error.
    assert.commandFailedWithCode(
        st.s.adminCommand(
            {commitTransaction: 1, lsid: lsid, txnNumber: NumberLong(0), autocommit: false}),
        ErrorCodes.NoSuchTransaction);

    let res = assert.commandFailedWithCode(st.s.getDB(kDbName).runCommand({
        insert: kCollName,
        documents: [{_id: -1 * docVal}],
        lsid: lsid,
        txnNumber: NumberLong(1),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
                                           ErrorCodes.InternalTransactionNotSupported);
    assert(ErrorCodes.isRetriableError(res.writeErrors[0].code));

    docVal++;
}

const testFuncs = [runTest];

for (const testFunc of testFuncs) {
    const lsidCombinations = [
        {
            id: UUID(),
            txnUUID: UUID(),
        },
        {id: UUID(), txnUUID: UUID(), txnNumber: NumberLong(0)}
    ];

    for (const lsid of lsidCombinations) {
        testFunc(lsid);
    }
}

st.stop();
})();
