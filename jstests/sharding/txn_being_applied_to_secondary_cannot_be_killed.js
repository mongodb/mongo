/**
 * Tests that the periodic transaction abort job cannot kill transactions being
 * applied on secondaries.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, uses_multi_shard_transaction]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {checkDecisionIs} from "jstests/sharding/libs/txn_two_phase_commit_util.js";

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

// Set to something high enough that a slow machine shouldn't cause our
// transaction to be aborted before committing, but low enough that the test
// won't be unnecessarily slow when we wait for the periodic transaction
// abort job to run.
TestData.transactionLifetimeLimitSeconds = 30;

const rsOpts = {
    // Make secondaries unelectable.
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    settings: {chainingAllowed: false, electionTimeoutMillis: ReplSetTest.kForeverMillis},
};
let st = new ShardingTest({mongos: 2, shards: {rs0: rsOpts, rs1: rsOpts, rs2: rsOpts}});

const coordinator = st.shard0;
const participant1 = st.shard1;
const participant2 = st.shard2;

// Create a sharded collection with a chunk on each shard:
// shard0: [-inf, 0)
// shard1: [0, 10)
// shard2: [10, +inf)
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: coordinator.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: participant2.shardName}));
st.refreshCatalogCacheForNs(st.s, ns);

// These forced refreshes are not strictly necessary; they just prevent extra TXN log lines
// from the shards starting, aborting, and restarting the transaction due to needing to
// refresh after the transaction has started.
assert.commandWorked(coordinator.adminCommand({_flushRoutingTableCacheUpdates: ns}));
assert.commandWorked(participant1.adminCommand({_flushRoutingTableCacheUpdates: ns}));
assert.commandWorked(participant2.adminCommand({_flushRoutingTableCacheUpdates: ns}));

// Start a new session and start a transaction on that session.
const session = st.s.startSession();
session.startTransaction();

// Insert a document onto each shard to make this a cross-shard transaction.
let res = assert.commandWorked(
    session.getDatabase(dbName).runCommand({
        insert: collName,
        documents: [{_id: -5}, {_id: 5}, {_id: 15}],
    }),
);
const lsid = session.getSessionId();
const txnNumber = session.getTxnNumber_forTesting();

// Set a failpoint to make oplog application hang on one secondary after applying the
// operations in the transaction but before preparing the TransactionParticipant.
const applyOpsHangBeforePreparingTransaction = "applyOpsHangBeforePreparingTransaction";
const firstSecondary = st.rs0.getSecondary();
const failPoint = configureFailPoint(firstSecondary, applyOpsHangBeforePreparingTransaction);

const coordinatorPrimary = coordinator.rs.getPrimary();
const hangBeforeDeletingCoordinatorDocFp = configureFailPoint(
    coordinatorPrimary,
    "hangBeforeDeletingCoordinatorDoc",
    {},
    "alwaysOn",
);

// Commit the transaction, which will execute two-phase commit.
assert.commandWorked(session.commitTransaction_forTesting());

hangBeforeDeletingCoordinatorDocFp.wait();
const commitTimestamp = checkDecisionIs(coordinator, lsid, txnNumber, "commit");
hangBeforeDeletingCoordinatorDocFp.off();

jsTest.log("Verify that the transaction was committed on all shards.");
res = assert.commandWorked(
    st.s.getDB(dbName).runCommand({
        find: collName,
        readConcern: {level: "majority", afterClusterTime: commitTimestamp},
        maxTimeMS: 10000,
    }),
);
assert.eq(3, res.cursor.firstBatch.length);

jsTest.log("Waiting for secondary to apply the prepare oplog entry.");
failPoint.wait();

// Wait for the periodic transaction abort job to run while oplog
// application is hanging. The job should run every 30 seconds due to the
// transactionLifetimeLimitSeconds parameter being set to 30 above, so the
// likelihood of it running while sleeping 60 seconds is high. If it does
// not run, the test will trivially pass without testing the desired
// behavior, but it will not cause the test to fail.
sleep(2 * TestData.transactionLifetimeLimitSeconds);

jsTest.log("Turning off " + applyOpsHangBeforePreparingTransaction + " failpoint.");
// Allow oplog application to continue by turning off the failpoint. The
// transaction should prepare successfully and should not have been aborted
// by the transaction abort job.
failPoint.off();

jsTest.log("Turned off " + applyOpsHangBeforePreparingTransaction + " failpoint.");

st.stop();
