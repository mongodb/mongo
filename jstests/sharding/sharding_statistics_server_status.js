//
// Tests that serverStatus includes sharding statistics by default and the sharding statistics are
// indeed the correct values. Does not test the catalog cache portion of sharding statistics.
//
// @tags: [
//     uses_transactions,
// ]

(function() {
'use strict';

load("jstests/libs/chunk_manipulation_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/wait_for_command.js");

function ShardStat() {
    this.countDonorMoveChunkStarted = 0;
    this.countRecipientMoveChunkStarted = 0;
    this.countDocsClonedOnRecipient = 0;
    this.countDocsClonedOnDonor = 0;
    this.countDocsDeletedOnDonor = 0;
}

function incrementStatsAndCheckServerShardStats(donor, recipient, numDocs) {
    ++donor.countDonorMoveChunkStarted;
    donor.countDocsClonedOnDonor += numDocs;
    ++recipient.countRecipientMoveChunkStarted;
    recipient.countDocsClonedOnRecipient += numDocs;
    donor.countDocsDeletedOnDonor += numDocs;
    const statsFromServerStatus = shardArr.map(function(shardVal) {
        return shardVal.getDB('admin').runCommand({serverStatus: 1}).shardingStatistics;
    });
    for (let i = 0; i < shardArr.length; ++i) {
        assert(statsFromServerStatus[i]);
        assert(statsFromServerStatus[i].countStaleConfigErrors);
        assert(statsFromServerStatus[i].totalCriticalSectionCommitTimeMillis);
        assert(statsFromServerStatus[i].totalCriticalSectionTimeMillis);
        assert(statsFromServerStatus[i].totalDonorChunkCloneTimeMillis);
        assert(statsFromServerStatus[i].countDonorMoveChunkLockTimeout);
        assert(statsFromServerStatus[i].countDonorMoveChunkAbortConflictingIndexOperation);
        assert.eq(stats[i].countDonorMoveChunkStarted,
                  statsFromServerStatus[i].countDonorMoveChunkStarted);
        assert.eq(stats[i].countDocsClonedOnRecipient,
                  statsFromServerStatus[i].countDocsClonedOnRecipient);
        assert.eq(stats[i].countDocsClonedOnDonor, statsFromServerStatus[i].countDocsClonedOnDonor);
        assert.eq(stats[i].countDocsDeletedOnDonor,
                  statsFromServerStatus[i].countDocsDeletedOnDonor);
        assert.eq(stats[i].countRecipientMoveChunkStarted,
                  statsFromServerStatus[i].countRecipientMoveChunkStarted);
    }
}

function checkServerStatusMigrationLockTimeoutCount(shardConn, count) {
    const shardStats =
        assert.commandWorked(shardConn.adminCommand({serverStatus: 1})).shardingStatistics;
    assert(shardStats.hasOwnProperty("countDonorMoveChunkLockTimeout"));
    assert.eq(count, shardStats.countDonorMoveChunkLockTimeout);
}

function checkServerStatusAbortedMigrationCount(shardConn, count) {
    const shardStats =
        assert.commandWorked(shardConn.adminCommand({serverStatus: 1})).shardingStatistics;
    assert(shardStats.hasOwnProperty("countDonorMoveChunkAbortConflictingIndexOperation"));
    assert.eq(count, shardStats.countDonorMoveChunkAbortConflictingIndexOperation);
}

function runConcurrentMoveChunk(host, ns, toShard) {
    const mongos = new Mongo(host);
    // Helper function to run moveChunk, retrying on ConflictingOperationInProgress. We need to
    // retry on ConflictingOperationInProgress to handle the following case:
    // 1. One test case does a moveChunk, expecting it to fail. It fails and completes on the donor
    // and returns to the test, while the recipient is still lagging for some reason and has not
    // completed.
    // 2. In the next test case, we attempt a moveChunk involving the same chunk and shards, but the
    // previous moveChunk is still in progress on the recipient shard from the previous migration,
    // causing this new moveChunk to return ConflictingOperationInProgress.
    //
    // This is expected behavior, so we retry until success or until some other unexpected error
    // occurs.
    function runMoveChunkUntilSuccessOrUnexpectedError() {
        let result = mongos.adminCommand({moveChunk: ns, find: {_id: 1}, to: toShard});
        let shouldRetry = (result.hasOwnProperty("code") &&
                           result.code == ErrorCodes.ConflictingOperationInProgress);
        if (shouldRetry) {
            jsTestLog("Retrying moveChunk due to ConflictingOperationInProgress");
        } else if (!result.ok) {
            jsTestLog("moveChunk encountered an error: " + tojson(result));
        }

        return shouldRetry ? runMoveChunkUntilSuccessOrUnexpectedError() : result;
    }
    // Kick off the recursive helper function.
    return runMoveChunkUntilSuccessOrUnexpectedError();
}

/**
 * Set a MODE_IS collection lock on 'collectionNs' to be held for 1 hour. This will ensure that the
 * lock will not be released before desired. The operation can be killed later to release the lock.
 *
 * 'sleepComment' adds a comment so that the operation is can be identified for waitForCommand().
 */
function sleepFunction(host, collectionNs, sleepComment) {
    const mongo = new Mongo(host);
    // Set a MODE_IS collection lock to be held for 1 hours.
    // Holding this lock for 1 hour will trigger a test timeout.
    assert.commandFailedWithCode(
        mongo.adminCommand(
            {sleep: 1, secs: 3600, lockTarget: collectionNs, lock: "ir", $comment: sleepComment}),
        ErrorCodes.Interrupted);
}

const dbName = "db";
const collName = "coll";

const st = new ShardingTest({shards: 2, mongos: 1});
const mongos = st.s0;
const admin = mongos.getDB("admin");
const coll = mongos.getCollection(dbName + "." + collName);
const numDocsToInsert = 3;
const shardArr = [st.shard0, st.shard1];
const stats = [new ShardStat(), new ShardStat()];
const index = {
    x: 1
};
let numDocsInserted = 0;

assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
st.ensurePrimaryShard(coll.getDB() + "", st.shard0.shardName);
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));

// Move chunk from shard0 to shard1 without docs.
assert.commandWorked(
    mongos.adminCommand({moveChunk: coll + '', find: {_id: 1}, to: st.shard1.shardName}));
incrementStatsAndCheckServerShardStats(stats[0], stats[1], numDocsInserted);

// Insert docs and then move chunk again from shard1 to shard0.
for (let i = 0; i < numDocsToInsert; ++i) {
    assert.commandWorked(coll.insert({_id: i}));
    ++numDocsInserted;
}
assert.commandWorked(mongos.adminCommand(
    {moveChunk: coll + '', find: {_id: 1}, to: st.shard0.shardName, _waitForDelete: true}));
incrementStatsAndCheckServerShardStats(stats[1], stats[0], numDocsInserted);

// Check that numbers are indeed cumulative. Move chunk from shard0 to shard1.
assert.commandWorked(mongos.adminCommand(
    {moveChunk: coll + '', find: {_id: 1}, to: st.shard1.shardName, _waitForDelete: true}));
incrementStatsAndCheckServerShardStats(stats[0], stats[1], numDocsInserted);

// Move chunk from shard1 to shard0.
assert.commandWorked(mongos.adminCommand(
    {moveChunk: coll + '', find: {_id: 1}, to: st.shard0.shardName, _waitForDelete: true}));
incrementStatsAndCheckServerShardStats(stats[1], stats[0], numDocsInserted);

//
// Tests for the count of migrations aborting from lock timeouts.
//

// Lower migrationLockAcquisitionMaxWaitMS so migrations time out more quickly.
const donorConn = st.rs0.getPrimary();
const lockParameterRes = assert.commandWorked(
    donorConn.adminCommand({getParameter: 1, migrationLockAcquisitionMaxWaitMS: 1}));
const originalMigrationLockTimeout = lockParameterRes.migrationLockAcquisitionMaxWaitMS;
assert.commandWorked(
    donorConn.adminCommand({setParameter: 1, migrationLockAcquisitionMaxWaitMS: 2 * 1000}));

// Counter starts at 0.
checkServerStatusMigrationLockTimeoutCount(donorConn, 0);

// Pause a migration before entering the critical section.
pauseMoveChunkAtStep(donorConn, moveChunkStepNames.reachedSteadyState);
let moveChunkThread =
    new Thread(runConcurrentMoveChunk, st.s.host, dbName + "." + collName, st.shard1.shardName);
moveChunkThread.start();
waitForMoveChunkStep(donorConn, moveChunkStepNames.reachedSteadyState);

// Start a transaction and insert to the migrating chunk to block entering the critical section.
const session = mongos.startSession();
session.startTransaction();
assert.commandWorked(session.getDatabase(dbName)[collName].insert({_id: 5}));

// Unpause the migration and it should time out entering the critical section.
unpauseMoveChunkAtStep(donorConn, moveChunkStepNames.reachedSteadyState);
moveChunkThread.join();
assert.commandFailedWithCode(moveChunkThread.returnData(), ErrorCodes.LockTimeout);

// Clean up the transaction and verify the counter was incremented in serverStatus.
assert.commandWorked(session.abortTransaction_forTesting());

checkServerStatusMigrationLockTimeoutCount(donorConn, 1);

// Writes are blocked during the critical section, so insert a document into the chunk to be
// moved before the migration begins that can be read later.
assert.commandWorked(st.s.getDB(dbName)[collName].insert({_id: 5}));

// Pause a migration after entering the critical section, but before entering the commit phase.
pauseMoveChunkAtStep(donorConn, moveChunkStepNames.chunkDataCommitted);
moveChunkThread =
    new Thread(runConcurrentMoveChunk, st.s.host, dbName + "." + collName, st.shard1.shardName);
moveChunkThread.start();
waitForMoveChunkStep(donorConn, moveChunkStepNames.chunkDataCommitted);

// Use the sleep cmd to acquire the collection MODE_IS lock asynchronously so that the migration
// cannot commit.
const sleepComment = "Lock sleep";
const sleepCommand =
    new Thread(sleepFunction, st.rs0.getPrimary().host, dbName + "." + collName, sleepComment);
sleepCommand.start();

// Wait for the sleep command to start.
const sleepID =
    waitForCommand("sleepCmd",
                   op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == sleepComment),
                   donorConn.getDB("admin"));

try {
    // Unpause the migration and it should time out entering the commit phase.
    unpauseMoveChunkAtStep(donorConn, moveChunkStepNames.chunkDataCommitted);
    moveChunkThread.join();
    assert.commandFailedWithCode(moveChunkThread.returnData(), ErrorCodes.LockTimeout);
} finally {
    // Kill the sleep command in order to release the collection MODE_IS lock.
    assert.commandWorked(donorConn.getDB("admin").killOp(sleepID));
    sleepCommand.join();
}

// Verify the counter was incremented in serverStatus.
checkServerStatusMigrationLockTimeoutCount(donorConn, 2);

assert.commandWorked(donorConn.adminCommand(
    {setParameter: 1, migrationLockAcquisitionMaxWaitMS: originalMigrationLockTimeout}));

//
// Tests for the count of migrations aborted due to concurrent index operations.
//
// Counter starts at 0.
checkServerStatusAbortedMigrationCount(donorConn, 0);

// Pause a migration after cloning starts.
pauseMoveChunkAtStep(donorConn, moveChunkStepNames.startedMoveChunk);
moveChunkThread =
    new Thread(runConcurrentMoveChunk, st.s.host, dbName + "." + collName, st.shard1.shardName);
moveChunkThread.start();
waitForMoveChunkStep(donorConn, moveChunkStepNames.startedMoveChunk);

// Run an index command.
assert.commandWorked(coll.createIndexes([index]));

// Unpause the migration and verify that it gets aborted.
unpauseMoveChunkAtStep(donorConn, moveChunkStepNames.startedMoveChunk);
moveChunkThread.join();
assert.commandFailedWithCode(moveChunkThread.returnData(), ErrorCodes.Interrupted);

checkServerStatusAbortedMigrationCount(donorConn, 1);

// Pause a migration before entering the critical section.
pauseMoveChunkAtStep(donorConn, moveChunkStepNames.reachedSteadyState);
moveChunkThread =
    new Thread(runConcurrentMoveChunk, st.s.host, dbName + "." + collName, st.shard1.shardName);
moveChunkThread.start();
waitForMoveChunkStep(donorConn, moveChunkStepNames.reachedSteadyState);

// Run an index command.
assert.commandWorked(
    st.s.getDB(dbName).runCommand({collMod: collName, validator: {x: {$type: "string"}}}));

// Unpause the migration and verify that it gets aborted.
unpauseMoveChunkAtStep(donorConn, moveChunkStepNames.reachedSteadyState);
moveChunkThread.join();
assert.commandFailedWithCode(moveChunkThread.returnData(), ErrorCodes.Interrupted);

checkServerStatusAbortedMigrationCount(donorConn, 2);

st.stop();
})();
