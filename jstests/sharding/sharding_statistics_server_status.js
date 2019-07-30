//
// Tests that serverStatus includes sharding statistics by default and the sharding statistics are
// indeed the correct values. Does not test the catalog cache portion of sharding statistics.
//
// @tags: [uses_transactions]

(function() {
'use strict';

load("jstests/libs/chunk_manipulation_util.js");
load("jstests/libs/parallelTester.js");

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

function runConcurrentMoveChunk(host, ns, toShard) {
    const mongos = new Mongo(host);
    return mongos.adminCommand({moveChunk: ns, find: {_id: 1}, to: toShard});
}

function runConcurrentRead(host, dbName, collName) {
    const mongos = new Mongo(host);
    return mongos.getDB(dbName)[collName].find({_id: 5}).comment("concurrent read").itcount();
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
    assert.writeOK(coll.insert({_id: i}));
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

// Pause a read while it's holding locks so the migration can't commit.
assert.commandWorked(
    donorConn.adminCommand({configureFailPoint: "waitInFindBeforeMakingBatch", mode: "alwaysOn"}));
const concurrentRead = new Thread(runConcurrentRead, st.s.host, dbName, collName);
concurrentRead.start();
assert.soon(function() {
    const curOpResults = assert.commandWorked(donorConn.adminCommand({currentOp: 1}));
    return curOpResults.inprog.some(op => op["command"]["comment"] === "concurrent read");
});

// Unpause the migration and it should time out entering the commit phase.
unpauseMoveChunkAtStep(donorConn, moveChunkStepNames.chunkDataCommitted);
moveChunkThread.join();
assert.commandFailedWithCode(moveChunkThread.returnData(), ErrorCodes.LockTimeout);

// Let the read finish and verify the counter was incremented in serverStatus.
assert.commandWorked(
    donorConn.adminCommand({configureFailPoint: "waitInFindBeforeMakingBatch", mode: "off"}));
concurrentRead.join();
assert.eq(1, concurrentRead.returnData());

checkServerStatusMigrationLockTimeoutCount(donorConn, 2);

assert.commandWorked(donorConn.adminCommand(
    {setParameter: 1, migrationLockAcquisitionMaxWaitMS: originalMigrationLockTimeout}));

st.stop();
})();
