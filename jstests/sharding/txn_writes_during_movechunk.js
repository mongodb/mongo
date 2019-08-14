// @tags: [uses_transactions]
load('./jstests/libs/chunk_manipulation_util.js');

(function() {
'use strict';

let staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

let st = new ShardingTest({shards: 2});

assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard0.shardName);
assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.user', key: {_id: 1}}));

let coll = st.s.getDB('test').user;
assert.commandWorked(coll.insert({_id: 'updateMe'}));
assert.commandWorked(coll.insert({_id: 'deleteMe'}));

pauseMigrateAtStep(st.shard1, migrateStepNames.deletedPriorDataInRange);

let joinMoveChunk =
    moveChunkParallel(staticMongod, st.s0.host, {_id: 0}, null, 'test.user', st.shard1.shardName);

waitForMigrateStep(st.shard1, migrateStepNames.deletedPriorDataInRange);

let session = st.s.startSession();
let sessionDB = session.getDatabase('test');
let sessionColl = sessionDB.getCollection('user');

session.startTransaction();
sessionColl.insert({_id: 'insertMe'});
sessionColl.update({_id: 'updateMe'}, {$inc: {y: 1}});
sessionColl.remove({_id: 'deleteMe'});

pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);
unpauseMigrateAtStep(st.shard1, migrateStepNames.deletedPriorDataInRange);
waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);

let recipientColl = st.rs1.getPrimary().getDB('test').user;
assert.eq(null, recipientColl.findOne({_id: 'insertMe'}));
assert.eq({_id: 'updateMe'}, recipientColl.findOne({_id: 'updateMe'}));
assert.eq({_id: 'deleteMe'}, recipientColl.findOne({_id: 'deleteMe'}));

assert.commandWorked(session.commitTransaction_forTesting());

unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);
joinMoveChunk();

assert.eq({_id: 'insertMe'}, recipientColl.findOne({_id: 'insertMe'}));
assert.eq({_id: 'updateMe', y: 1}, recipientColl.findOne({_id: 'updateMe'}));
assert.eq(null, recipientColl.findOne({_id: 'deleteMe'}));

assert.eq(null, recipientColl.findOne({x: 1}));

st.stop();
MongoRunner.stopMongod(staticMongod);
})();
