/*
 * Test that the logical session cache reaper does not reap sessions with active TransactionRouter
 * yielders.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");

// This test runs the reapLogicalSessionCacheNow command. That can lead to direct writes to the
// config.transactions collection, which cannot be performed on a session.
TestData.disableImplicitSessions = true;

const st = new ShardingTest({
    shards: 2,
    rs: {
        setParameter: {
            TransactionRecordMinimumLifetimeMinutes: 0,
            // TODO (SERVER-67620): Lower log verbosity in reap_sessions_with_active_yielders.js
            logComponentVerbosity: tojson({transaction: {verbosity: 5}})
        }
    }
});
const shard0Primary = st.rs0.getPrimary();

// Set up a sharded collection with two chunks:
// shard0: [MinKey, 0]
// shard1: [0, MaxKey]
const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
const mongosTestDB = st.s.getDB(dbName);
const mongosTestColl = mongosTestDB.getCollection(collName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: ns, find: {x: MinKey}, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1}, to: st.shard1.shardName}));

const sessionsColl = st.s.getCollection("config.system.sessions");
const transactionsCollOnShard0 = shard0Primary.getCollection("config.transactions");
const imageCollOnShard0 = shard0Primary.getCollection("config.image_collection");

function assertNumEntries(
    {sessionUUID, numSessionsCollEntries, numTransactionsCollEntries, numImageCollEntries}) {
    const filter = {"_id.id": sessionUUID};
    assert.eq(numSessionsCollEntries,
              sessionsColl.find(filter).itcount(),
              tojson(sessionsColl.find().toArray()));
    assert.eq(numTransactionsCollEntries,
              transactionsCollOnShard0.find(filter).itcount(),
              tojson(transactionsCollOnShard0.find().toArray()));
    assert.eq(numImageCollEntries,
              imageCollOnShard0.find().itcount(),
              tojson(imageCollOnShard0.find().toArray()));
}

jsTest.log("Test reaping when there is an internal transaction with an active " +
           "TransactionRouter yielder");

const parentLsid = {
    id: UUID()
};

const runInternalTxn = (shard0PrimaryHost, parentLsidUUIDString, dbName, collName) => {
    const shard0Primary = new Mongo(shard0PrimaryHost);
    const testInternalTxnCmdObj = {
        testInternalTransactions: 1,
        commandInfos: [{
            dbName,
            command: {
                insert: collName,
                documents: [{x: -10}, {x: 10}],
                stmtIds: [NumberInt(1), NumberInt(2)]
            }
        }],
        useClusterClient: true,
        lsid: {id: UUID(parentLsidUUIDString)},
    };
    assert.commandWorked(shard0Primary.adminCommand(testInternalTxnCmdObj));
};

// Use {skip:2} to pause the transaction before unyielding after committing instead of after
// executing the two insert statements.
let fp = configureFailPoint(shard0Primary, "hangBeforeUnyieldingTransactionRouter", {}, {skip: 2});
const internalTxnThread = new Thread(
    runInternalTxn, shard0Primary.host, extractUUIDFromObject(parentLsid.id), dbName, collName);
internalTxnThread.start();
fp.wait();

assert.commandWorked(shard0Primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
assertNumEntries({
    sessionUUID: parentLsid.id,
    numSessionsCollEntries: 1,
    numTransactionsCollEntries: 1,
    numImageCollEntries: 0
});

// Force the logical session cache to reap, and verify that the config.transactions entry for
// the internal transaction does not get reaped.
assert.commandWorked(sessionsColl.remove({"_id.id": parentLsid.id}));
assert.commandWorked(shard0Primary.adminCommand({reapLogicalSessionCacheNow: 1}));
assertNumEntries({
    sessionUUID: parentLsid.id,
    numSessionsCollEntries: 0,
    numTransactionsCollEntries: 1,
    numImageCollEntries: 0
});

fp.off();
internalTxnThread.join();

assert.eq(mongosTestColl.find({x: -10}).itcount(), 1);
assert.eq(mongosTestColl.find({x: 10}).itcount(), 1);

assert.commandWorked(shard0Primary.adminCommand({reapLogicalSessionCacheNow: 1}));
assertNumEntries({
    sessionUUID: parentLsid.id,
    numSessionsCollEntries: 0,
    numTransactionsCollEntries: 0,
    numImageCollEntries: 0
});

st.stop();
})();
