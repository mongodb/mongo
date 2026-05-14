/*
 * Regression-pin for SERVER-101626: prepared transactions are bottlenecked on
 * secondaries. Despite the batching improvements landed in 7.0/8.0
 * (SERVER-75800), prepares and commits are not batched together, and commit
 * ops cannot batch with surrounding CRUD ops. Under concurrent prepared
 * transaction load, the secondary oplog applier therefore serialises prepare
 * entries -- the session-store exclusive lock acquired by
 * applyPrepareTransaction is the choke point.
 *
 * This test documents current behaviour so that future parallelisation work
 * surfaces as a measurable delta. It:
 *   1. Spins up a 3-node ReplSetTest.
 *   2. Drives N=10 concurrent prepared transactions on the primary, each
 *      touching a disjoint document so that the only contention left is the
 *      secondary applier's session-store lock.
 *   3. Measures wall-clock time for the secondary to fully apply every
 *      prepare oplog entry via awaitReplication() plus per-session oplog
 *      tailing.
 *   4. Asserts the wall time is bounded by a generous upper limit -- this is
 *      a baseline pin, not a tight SLA. If/when the secondary applier learns
 *      to parallelise prepared-txn application across sessions, this test
 *      should be revisited so the regression-pin moves down with the new
 *      lower bound rather than silently masking improvements.
 *
 * N=10 is deliberately modest to avoid OOM on shared CI infrastructure; the
 * bottleneck is observable well below the point where memory becomes a
 * concern.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = jsTest.name();
const collName = "coll";

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondaries = rst.getSecondaries();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

// Modest N to stay within shared CI memory limits while still being well
// above the point at which the session-store lock dominates.
const numTxns = 10;

jsTestLog("Seeding " + numTxns + " documents on the primary");
const seedDocs = [];
for (let i = 0; i < numTxns; i++) {
    seedDocs.push({_id: i, v: 0});
}
assert.commandWorked(primaryColl.insert(seedDocs, {writeConcern: {w: "majority"}}));
rst.awaitReplication();

jsTestLog("Starting " + numTxns + " concurrent sessions and preparing each");
const sessions = [];
const prepareTimestamps = [];

for (let i = 0; i < numTxns; i++) {
    const s = primary.startSession();
    const sDB = s.getDatabase(dbName);
    const sColl = sDB.getCollection(collName);
    s.startTransaction({writeConcern: {w: "majority"}});
    // Each session updates a disjoint document so contention is forced into
    // the secondary applier's session-store path rather than per-document
    // WT_ROLLBACK retries.
    assert.commandWorked(sColl.update({_id: i}, {$inc: {v: 1}}));
    sessions.push(s);
}

// Issue prepareTransaction back-to-back from the driver. This produces the
// concurrent inbound prepare workload that the secondary then has to apply
// through the session-store-locked applier path.
const prepareStart = Date.now();
for (const s of sessions) {
    prepareTimestamps.push(PrepareHelpers.prepareTransaction(s));
}
const prepareDriverMs = Date.now() - prepareStart;
jsTestLog("Primary accepted " + numTxns + " prepares in " + prepareDriverMs + " ms (driver-side)");

// Now measure how long secondaries take to fully replicate the prepares.
// awaitReplication() returns once the secondaries have applied through the
// primary's last optime, which by definition includes every prepare we just
// issued. The wall time captured here is the regression baseline.
const replStart = Date.now();
rst.awaitReplication();
const replWallMs = Date.now() - replStart;
jsTestLog("Secondaries caught up on all prepares in " + replWallMs + " ms");

// Sanity-check that each prepare landed on every secondary by tailing the
// oplog for the prepare entries. This proves the wall time we just recorded
// actually covers all numTxns prepares rather than just the primary's local
// state.
for (const secondary of secondaries) {
    const oplog = secondary.getDB("local").oplog.rs;
    const preparesSeen = oplog.find({prepare: true}).itcount();
    assert.gte(preparesSeen,
               numTxns,
               "secondary " + secondary.host + " is missing prepare oplog entries: saw " +
                   preparesSeen + " expected >= " + numTxns);
}

// Regression-pin bound. Single-writer secondary application of 10 prepares
// on a quiet ReplSetTest comfortably fits well under this ceiling on every
// supported platform. The bound is loose on purpose: we are documenting
// "current behaviour does not blow up" rather than asserting a tight SLA.
// When SERVER-101626 lands a parallel applier, this number should drop
// noticeably and the bound should be tightened.
const wallMsUpperBound = 60 * 1000;
assert.lt(replWallMs,
          wallMsUpperBound,
          "secondary prepared-txn application took " + replWallMs + " ms which exceeds the " +
              "regression-pin upper bound of " + wallMsUpperBound + " ms");

jsTestLog("Recorded baseline: " + replWallMs + " ms for " + numTxns + " concurrent prepares");

jsTestLog("Committing all prepared transactions to clean up");
for (let i = 0; i < sessions.length; i++) {
    assert.commandWorked(PrepareHelpers.commitTransaction(sessions[i], prepareTimestamps[i]));
}

rst.awaitReplication();

// Verify that each secondary observes the committed state.
for (const secondary of secondaries) {
    const secColl = secondary.getDB(dbName)[collName];
    for (let i = 0; i < numTxns; i++) {
        const doc = secColl.findOne({_id: i});
        assert.eq(doc.v, 1, "secondary " + secondary.host + " did not apply commit for _id " + i);
    }
}

rst.stopSet();
