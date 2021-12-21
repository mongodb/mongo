/**
 * This test validates that if a shard donates a chunk immediately after a receive of another chunk
 * has completed, but before the original donor has committed the metadata change, we will not end
 * up with gaps in the metadata.
 *
 * @tags: [
 *   # SERVER-62181 avoid CS stepdowns, since they can cause the migrations
 *   # (in combination with their failpoints) issued by this test to enter a deadlock
 *   does_not_support_stepdowns,
 *  ]
 */

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
'use strict';

var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

var st = new ShardingTest({shards: 3});

const featureFlagMigrationRecipientCriticalSection =
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {getParameter: 1, featureFlagMigrationRecipientCriticalSection: 1}));
const featureFlagMigrationRecipientCriticalSectionEnabled =
    featureFlagMigrationRecipientCriticalSection.featureFlagMigrationRecipientCriticalSection.value;

if (featureFlagMigrationRecipientCriticalSectionEnabled) {
    // When featureFlagMigrationRecipientCriticalSection is enabled, the recipient cannot start a
    // new migration until the donor has instructed it to release its critical section (after the
    // commit on the configsvr). Because of this, this test is not possible.
    jsTest.log('Skipping test because featureFlagMigrationRecipientCriticalSection is enabled');
    st.stop();
    MongoRunner.stopMongod(staticMongod);
    return;
}

assert.commandWorked(st.s0.adminCommand({enableSharding: 'TestDB'}));
st.ensurePrimaryShard('TestDB', st.shard0.shardName);
assert.commandWorked(st.s0.adminCommand({shardCollection: 'TestDB.TestColl', key: {Key: 1}}));

var testDB = st.s0.getDB('TestDB');
var testColl = testDB.TestColl;

// Create 3 chunks with one document each and move them so that 0 is on shard0, 1 is on shard1,
// etc.
assert.commandWorked(testColl.insert({Key: 0, Value: 'Value'}));
assert.commandWorked(testColl.insert({Key: 100, Value: 'Value'}));
assert.commandWorked(testColl.insert({Key: 101, Value: 'Value'}));
assert.commandWorked(testColl.insert({Key: 200, Value: 'Value'}));

assert.commandWorked(st.s0.adminCommand({split: 'TestDB.TestColl', middle: {Key: 100}}));
assert.commandWorked(st.s0.adminCommand({split: 'TestDB.TestColl', middle: {Key: 101}}));
assert.commandWorked(st.s0.adminCommand({split: 'TestDB.TestColl', middle: {Key: 200}}));

assert.commandWorked(st.s0.adminCommand({
    moveChunk: 'TestDB.TestColl',
    find: {Key: 100},
    to: st.shard1.shardName,
    _waitForDelete: true
}));
assert.commandWorked(st.s0.adminCommand({
    moveChunk: 'TestDB.TestColl',
    find: {Key: 101},
    to: st.shard1.shardName,
    _waitForDelete: true
}));
assert.commandWorked(st.s0.adminCommand({
    moveChunk: 'TestDB.TestColl',
    find: {Key: 200},
    to: st.shard2.shardName,
    _waitForDelete: true
}));

// Start moving chunk 0 from shard0 to shard1 and pause it just before the metadata is written
// (but after the migration of the documents has been committed on the recipient)
pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.chunkDataCommitted);
var joinMoveChunk0 = moveChunkParallel(
    staticMongod, st.s0.host, {Key: 0}, null, 'TestDB.TestColl', st.shard1.shardName);
waitForMoveChunkStep(st.shard0, moveChunkStepNames.chunkDataCommitted);

pauseMoveChunkAtStep(st.shard1, moveChunkStepNames.chunkDataCommitted);
var joinMoveChunk1 = moveChunkParallel(
    staticMongod, st.s0.host, {Key: 100}, null, 'TestDB.TestColl', st.shard2.shardName);
waitForMoveChunkStep(st.shard1, moveChunkStepNames.chunkDataCommitted);

unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.chunkDataCommitted);
unpauseMoveChunkAtStep(st.shard1, moveChunkStepNames.chunkDataCommitted);

joinMoveChunk0();
joinMoveChunk1();

var foundDocs = testColl.find().toArray();
assert.eq(4, foundDocs.length, 'Incorrect number of documents found ' + tojson(foundDocs));

st.stop();
MongoRunner.stopMongod(staticMongod);
})();
