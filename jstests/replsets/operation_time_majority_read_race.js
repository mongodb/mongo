/**
 * Ensures that the operationTime for majority reads is reported as the actual read timestamp.
 * A previous bug occurred because getCurrentCommittedSnapshotOpTime() can advance on another
 * thread between when the read executes and when operationTime is computed, causing the response
 * to report a timestamp newer than what the read actually observed.
 *
 * This can cause real data inconsistency via causal consistency: a client uses operationTime as
 * afterClusterTime on a follow-up majority read. With the bug, operationTime is advanced past a
 * concurrent update the original read never saw, so the follow-up is required to show that update.
 * From the client's perspective, a field appears to change between two consecutive reads with no
 * intervening write on the client's part.
 *
 * This test uses the hangBeforeComputeOperationTimeForMajorityRead failpoint to block the read
 * command before it computes operationTime, allowing the committed snapshot to advance via a
 * concurrent update. With the bug, operationTime is reported as >= the update commit timestamp
 * even though the read's snapshot only included the pre-update value.
 *
 * @tags: [
 *     requires_majority_read_concern,
 *     requires_fcv_83,
 *     uses_transactions,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = jsTestName();
const collectionName = "coll";

const rst = new ReplSetTest({name: dbName, nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB(dbName);
const fullNs = `${dbName}.${collectionName}`;

// Establish a baseline: insert a document and wait for replication.
assert.commandWorked(
    testDB.runCommand({
        insert: collectionName,
        documents: [{_id: 1, x: 1}],
        writeConcern: {w: "majority"},
    }),
);
rst.awaitReplication();

// Configure the failpoint to block our find before it computes operationTime.
// The failpoint must be namespace-specific to avoid affecting other tests.
const failpoint = configureFailPoint(primary, "hangBeforeComputeOperationTimeForMajorityRead", {
    ns: fullNs,
});

// Run a majority read in a parallel shell. It will block at the failpoint after
// executing the read but before computing operationTime. Writes the result to a
// collection since startParallelShell returns the exit code, not the function's return value.
const resultCollName = "operation_time_race_result";
function runMajorityFind(host, dbName, collectionName, resultCollName) {
    const res = assert.commandWorked(
        new Mongo(host).getDB(dbName).runCommand({
            find: collectionName,
            filter: {},
            readConcern: {level: "majority"},
        }),
    );
    // Write result to collection so main thread can read it (parallel shell returns exit code only).
    new Mongo(host).getDB(dbName).getCollection(resultCollName).drop();
    new Mongo(host).getDB(dbName).getCollection(resultCollName).insertOne({
        operationTime: res.operationTime,
        firstBatch: res.cursor.firstBatch,
    });
}

const parallelResult = startParallelShell(
    funWithArgs(runMajorityFind, primary.host, dbName, collectionName, resultCollName),
    primary.port,
);

// Wait for the failpoint to be hit (the find has executed the read and is about to compute
// operationTime).
failpoint.wait();

// While the find is blocked, update the document to advance the committed snapshot.
// The read's snapshot was established before this update, so the read observed x=1.
// With the bug, computeOperationTime will pick up this newer committed snapshot and
// report operationTime >= the update timestamp, even though the read never saw x=2.
const updateRes = assert.commandWorked(
    testDB.runCommand({
        update: collectionName,
        updates: [{q: {_id: 1}, u: {$set: {x: 2}}}],
        writeConcern: {w: "majority"},
    }),
);
const writeCommitTimestamp = updateRes.operationTime;

rst.awaitReplication();

// Verify the update has advanced the committed snapshot.
const statusAfterWrite = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
assert.gte(
    timestampCmp(statusAfterWrite.optimes.lastCommittedOpTime.ts, writeCommitTimestamp),
    0,
    "Update should have advanced the committed snapshot: lastCommittedOpTime (" +
        tojson(statusAfterWrite.optimes.lastCommittedOpTime.ts) +
        ") should be >= update commit timestamp (" +
        tojson(writeCommitTimestamp) +
        ")",
);

const commitPointAfterUpdate = writeCommitTimestamp;

// Release the failpoint so the find can complete.
failpoint.off();

parallelResult();

// Read the result that the parallel shell wrote to the collection.
const findResult = testDB.getCollection(resultCollName).findOne();
assert(findResult, "Parallel shell should have written find result to " + resultCollName);

// The read executed before the update, so it must have seen x=1.
const firstBatch = findResult.firstBatch ?? [];
assert.eq(firstBatch.length, 1);
assert.eq(firstBatch[0]._id, 1);
assert.eq(
    firstBatch[0].x,
    1,
    "Majority read must see x=1: the snapshot was established before " + "the update to x=2 and should not reflect it",
);

// Demonstrate the causal inconsistency by reading at exactly operationTime using atClusterTime
// in a snapshot transaction. If operationTime correctly reflects the read's snapshot (with fix),
// reading at that exact timestamp must also return x=1. With the bug, operationTime is advanced
// to the update's commit timestamp, so reading at atClusterTime=operationTime returns x=2 —
// inconsistent with the x=1 the majority read returned at "the same" timestamp.
assert(findResult.operationTime !== undefined, "Find result should include operationTime");
const session = primary.startSession({causalConsistency: false});
session.startTransaction({readConcern: {level: "snapshot", atClusterTime: findResult.operationTime}});
const followUpResult = assert.commandWorked(
    session.getDatabase(dbName).runCommand({find: collectionName, filter: {_id: 1}}),
);
const followUpX = followUpResult.cursor.firstBatch[0].x;
session.abortTransaction();
session.endSession();
assert.eq(
    followUpX,
    1,
    "Reading at atClusterTime=" +
        tojson(findResult.operationTime) +
        " (operationTime from the majority read) returned x=" +
        followUpX +
        " but the majority read returned x=1. If operationTime correctly reflects the read's" +
        " snapshot, reading at that exact timestamp must also return x=1. With the bug," +
        " operationTime is advanced to the update timestamp (" +
        tojson(commitPointAfterUpdate) +
        "), so reading at atClusterTime=operationTime" +
        " returns the post-update x=2.",
);

// BUG ASSERTION: operationTime must be < the update commit timestamp.
// The read saw x=1 (pre-update), so operationTime must reflect a snapshot before the update.
// With the bug, operationTime is advanced to the update timestamp, making the follow-up read
// return x=2 even though the first read returned x=1 — a causal inconsistency from the
// client's perspective.
assert.lt(
    timestampCmp(findResult.operationTime, commitPointAfterUpdate),
    0,
    "operationTime (" +
        tojson(findResult.operationTime) +
        ") must be less than the update commit timestamp (" +
        tojson(commitPointAfterUpdate) +
        "). The read returned x=1 (pre-update), so operationTime must precede the update. " +
        "With the bug, a causally-consistent follow-up read returns x=" +
        followUpX +
        " while the original read returned x=" +
        firstBatch[0].x +
        ", making x appear to change between two consecutive reads with no client write.",
);

testDB.getCollection(resultCollName).drop();
rst.stopSet();
