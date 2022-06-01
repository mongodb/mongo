/*
 * Tests that sessions used for non-retryable internal transactions are pooled.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
"use strict";

const st = new ShardingTest({shards: 1, config: 1});
const shard0Primary = st.rs0.getPrimary();

const kDbName = "testDb";
const kCollName = "testColl";
const mongosTestColl = st.s.getCollection(kDbName + "." + kCollName);
assert.commandWorked(mongosTestColl.insert({x: 1}));  // Set up the collection.

const sessionsColl = st.s.getCollection("config.system.sessions");
const transactionsCollOnShard = shard0Primary.getCollection("config.transactions");

function assertNumEntries({sessionUUID, numSessionsCollEntries, numTransactionsCollEntries}) {
    const filter = {"_id.id": sessionUUID};
    assert.eq(numSessionsCollEntries,
              sessionsColl.find(filter).itcount(),
              tojson(sessionsColl.find().toArray()));
    assert.eq(numTransactionsCollEntries,
              transactionsCollOnShard.find(filter).itcount(),
              tojson(transactionsCollOnShard.find().toArray()));
}

function runInternalTxn(conn, lsid) {
    const testInternalTxnCmdObj = {
        testInternalTransactions: 1,
        commandInfos:
            [{dbName: kDbName, command: {insert: kCollName, documents: [{x: -10}, {x: 10}]}}],
        lsid: lsid,
    };
    assert.commandWorked(conn.adminCommand(testInternalTxnCmdObj));
}

function runTest(conn) {
    const parentLsid = {id: UUID()};

    runInternalTxn(conn, parentLsid);

    assert.commandWorked(conn.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.commandWorked(shard0Primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        {sessionUUID: parentLsid.id, numSessionsCollEntries: 1, numTransactionsCollEntries: 1});

    // Run more transactions and verify the number of sessions used doesn't change.

    runInternalTxn(conn, parentLsid);
    runInternalTxn(conn, parentLsid);
    runInternalTxn(conn, parentLsid);

    assert.commandWorked(conn.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.commandWorked(shard0Primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        {sessionUUID: parentLsid.id, numSessionsCollEntries: 1, numTransactionsCollEntries: 1});

    // Verify other sessions can be pooled concurrently.

    const otherParentLsid1 = {id: UUID()};
    const otherParentLsid2 = {id: UUID()};

    runInternalTxn(conn, otherParentLsid1);
    runInternalTxn(conn, otherParentLsid2);
    runInternalTxn(conn, otherParentLsid2);
    runInternalTxn(conn, otherParentLsid1);
    runInternalTxn(conn, otherParentLsid2);
    runInternalTxn(conn, otherParentLsid2);
    runInternalTxn(conn, otherParentLsid1);

    runInternalTxn(conn, parentLsid);

    assert.commandWorked(conn.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.commandWorked(shard0Primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assertNumEntries(
        {sessionUUID: parentLsid.id, numSessionsCollEntries: 1, numTransactionsCollEntries: 1});
    assertNumEntries({
        sessionUUID: otherParentLsid1.id,
        numSessionsCollEntries: 1,
        numTransactionsCollEntries: 1
    });
    assertNumEntries({
        sessionUUID: otherParentLsid2.id,
        numSessionsCollEntries: 1,
        numTransactionsCollEntries: 1
    });
}

runTest(st.s);

st.stop();
})();
