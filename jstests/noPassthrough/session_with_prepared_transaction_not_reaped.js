/**
 * This test makes sure that secondaries will not reap sessions when there is still a prepared
 * transaction on it.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";

// Start the sharding test and add the replica set.
const st = new ShardingTest({
    shards: 1,
    rs: {nodes: [{}, {rsConfig: {priority: 0}}]},
    other: {
        rsOptions: {
            setParameter: {
                ttlMonitorEnabled: false,
                disableLogicalSessionCacheRefresh: false,
                logicalSessionRefreshMillis: 1000 * 60 * 60 *
                    24,  // 24 hours, to avoid refreshing the cache before the test calls it
                TransactionRecordMinimumLifetimeMinutes: 0,
                localLogicalSessionTimeoutMinutes: 0,
                internalSessionsReapThreshold: 5,
            }
        }
    }
});
assert.commandWorked(st.s.adminCommand({shardCollection: "config.system.sessions", key: {_id: 1}}));

const rst = st.rs0;
const primary = rst.getPrimary();
const secondary = rst.getSecondary();

const dbName = jsTestName();
const collName = "testColl";

const testDB = primary.getDB(dbName);
const testDBSecondary = secondary.getDB(dbName);

assert.commandWorked(testDB.createCollection(collName));

const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB[collName];

// Perform a write inside of a prepared transaction.
session.startTransaction();
assert.commandWorked(sessionColl.insert({x: 1}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

// Check how many sessions have been reaped by the logical session cache on the
// secondary before calling reapLogicalSessionCacheNow.
let serverStatus = assert.commandWorked(secondary.adminCommand({serverStatus: 1}));
const numSessionsReapedBefore =
    serverStatus.logicalSessionRecordCache.lastTransactionReaperJobEntriesCleanedUp;
const numReaperJobCountBefore = serverStatus.logicalSessionRecordCache.transactionReaperJobCount;

// Refreshing the logical session cache on the secondary. This will try to reap
// sessions, but it should not reap the session with prepared transaction. So
// calling refreshLogicalSessionCacheNow should not do anything to the
// transaction/session in this test.
jsTestLog("Reaping logical session cache now.");
assert.commandWorked(secondary.adminCommand({reapLogicalSessionCacheNow: 1}));

// Check that no sessions were reaped.
serverStatus = assert.commandWorked(secondary.adminCommand({serverStatus: 1}));
const numSessionsReapedAfter =
    serverStatus.logicalSessionRecordCache.lastTransactionReaperJobEntriesCleanedUp;
const numReaperJobCountAfter = serverStatus.logicalSessionRecordCache.transactionReaperJobCount;

// Make sure that the reaper actually ran.
assert.gte(1,
           numReaperJobCountAfter - numReaperJobCountBefore,
           "Reaper should have run at least once after the reapLogicalSessionCacheNow command.");
assert.eq(numSessionsReapedBefore, numSessionsReapedAfter, "No sessions should have been reaped.");

// Make sure that we can still commit the transaction after attempting to reap
// the session. Since the session has not been reaped, we should not run into
// any issues committing the transaction. (See SERVER-105751.)
PrepareHelpers.commitTransaction(session, prepareTimestamp);

session.endSession();

st.stop();
