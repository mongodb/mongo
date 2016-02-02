//
// This test validates that when a donor shard aborts a migration before it sends a commit to
// the recipient and then starts a different migration, the original recipient cannot retrieve
// transfer documents from the donor that are intended for a different shard. For more
// information see SERVER-20290.
//
// The sequence of events is:
//  - Start shards 1, 2 and 3 (s0, s1, s2), create two sharded collections with the same shardkey,
//    each with 2 chunks on s0.
//  - Insert documents into each collection's 2 chunks, so all chunks have a document.
//  - On the recipient shard (s1) enable the migrateThreadHangAtStep3 failpoint to pause
//    the migration after document cloning.
//  - Start migration of the first collection's chunk from s0 to s1. The recipient shard s1 will
//    block when it reaches the failpoint, so execute the moveChunk command on a separate thread.
//  - Abort the migration on the donor shard, s0.
//  - On the other recipient shard (s2) enable the migrateThreadHangAtStep3 failpoint, to pause
//    the migration after document cloning.
//  - Start migration of the other collection's chunk from s0 to s2. The recipient shard s2 will
//    block when it reaches the failpoint, so execute the moveChunk command on a separate thread.
//  - Now insert 2 new documents in the chunk being moved to s2 so the migration's xfermods log is
//    populated, and unpause the migrateThreadHangAtStep3 on s1. This will cause s1 to resume
//    fetching documents from s0, and s0 will refuse s1 access to the xfermods after checking s1's
//    session ID, which no longer matches the current migration.
//
// This tests migration session IDs, the reason for which is explained in SERVER-20290.
//

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
"use strict";

var staticMongodFoo = MongoRunner.runMongod({});  // For startParallelOps.
var staticMongodBaz = MongoRunner.runMongod({});  // For startParallelOps.

/**
 * Start up new sharded cluster, balancer defaults to off.
 */

var st = new ShardingTest({ shards : 3, mongos : 1 });

var mongos = st.s0,
    admin = mongos.getDB('admin'),
    shards = mongos.getCollection('config.shards').find().toArray(),
    dbName = "testDB",
    fooNS = dbName + ".foo",
    fooColl = mongos.getCollection(fooNS),
    bazNS = dbName + ".baz",
    bazColl = mongos.getCollection(bazNS),
    donor = st.shard0,
    fooRecipient = st.shard1,
    bazRecipient = st.shard2,
    fooDonorColl = donor.getCollection(fooNS),
    bazDonorColl = donor.getCollection(bazNS),
    fooRecipientColl = fooRecipient.getCollection(fooNS),
    bazRecipientColl = bazRecipient.getCollection(bazNS);

/**
 * Enable sharding on both collections, and split each collection into two chunks.
 */

// Two chunks
// Donor:
//      testDB.foo:     [0, 10) [10, 20)
//      testDB.baz:     [0, 10) [10, 20)
// Recipient:
assert.commandWorked(admin.runCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, shards[0]._id);

assert.commandWorked(admin.runCommand({shardCollection: fooNS, key: {a: 1}}));
assert.commandWorked(admin.runCommand({split: fooNS, middle: {a: 10}}));
assert.commandWorked(admin.runCommand({shardCollection: bazNS, key: {a: 1}}));
assert.commandWorked(admin.runCommand({split: bazNS, middle: {a: 10}}));

/**
 * Insert one document into each of the chunks in the testDB.baz and testDB.foo collections.
 */

assert.writeOK(fooColl.insert({a: 0}));
assert.writeOK(fooColl.insert({a: 10}));
assert.eq(0, fooRecipientColl.count());
assert.eq(2, fooDonorColl.count());
assert.eq(2, fooColl.count());
assert.writeOK(bazColl.insert({a: 0}));
assert.writeOK(bazColl.insert({a: 10}));
assert.eq(0, bazRecipientColl.count());
assert.eq(2, bazDonorColl.count());
assert.eq(2, bazColl.count());

/**
 * Set the failpoints. Both recipient shards will pause migration after cloning chunk
 * data from donor, and before checking transfer mods log on donor. Pause the donor shard
 * before it checks for interrupts to the migration.
 */

pauseMigrateAtStep(fooRecipient, migrateStepNames.cloned);
pauseMigrateAtStep(bazRecipient, migrateStepNames.cloned);
pauseMoveChunkAtStep(donor, moveChunkStepNames.startedMoveChunk);

/**
 * Start first moveChunk operation in the background: moving chunk [10, 20) in testDB.foo
 * from donor to fooRecipient. This will move one document, {a: 10}. Migration will pause
 * after cloning step (when it reaches the failpoint).
 */

// Donor:   testDB.foo [10, 20) -> FooRecipient
//      testDB.foo:     [0, 10)
jsTest.log('Starting first migration of collection foo, pause after cloning...');
var joinFooMoveChunk = moveChunkParallel(
    staticMongodFoo,
    st.s0.host,
    {a: 10},
    null,
    fooColl.getFullName(),
    shards[1]._id);
waitForMigrateStep(fooRecipient, migrateStepNames.cloned);

/**
 * Abort the migration on the donor shard by finding and killing the operation by operation
 * ID. Release the donor shard failpoint so that the donor shard can discover the migration
 * has received a interrupt signal. The recipient shard, fooRecipient, which is currently
 * paused, will not yet be aware that the migration has been aborted.
 */

jsTest.log('Abort donor shard migration of foo collection....');
var inProgressOps = admin.currentOp().inprog;
for (var op in inProgressOps) {
    if (inProgressOps[op].query.moveChunk) {
        admin.killOp(inProgressOps[op].opid);
        jsTest.log("Killing migration with opid: " + inProgressOps[op].opid);
    }
}
unpauseMoveChunkAtStep(donor, moveChunkStepNames.startedMoveChunk);

/**
 * Start second moveChunk operation in the background: moving chunk [10, 20) in testDB.baz
 * from donor to bazRecipient. This will move one document, {a: 10}. Migration will pause
 * after the recipient cloning step (when it reaches the failpoint).
 */

// Donor:   testDB.baz [10, 20) -> BazRecipient
//      testDB.baz:     [0, 10)
jsTest.log('Starting second migration of collection baz, pause after cloning...');
var joinBazMoveChunk = moveChunkParallel(
    staticMongodBaz,
    st.s0.host,
    {a: 10},
    null,
    bazColl.getFullName(),
    shards[2]._id);
waitForMigrateStep(bazRecipient, migrateStepNames.cloned);

/**
 * Insert documents into testDB.baz collection's currently migrating chunk with range
 * [10, 20) so as to populate the migration xfermods log.
 */

jsTest.log("Inserting 2 docs into donor shard's testDB.baz collection " +
    "in the range of the currently migrating chunk....");
assert.writeOK(bazColl.insert({a: 11}));
assert.writeOK(bazColl.insert({a: 12}));
assert.eq(4, bazColl.count(), "Failed to insert documents into baz collection!");

/**
 * Unpause fooRecipient (disable failpoint) and finish first migration, which should fail.
 * FooRecipient will be attempting to access the donor shard's migration xfermods log,
 * which has documents but for a different migration. FooRecipient will fail to get retrieve
 * the documents, and abort the migration.
 */

jsTest.log('Finishing first migration, which should fail....');
unpauseMigrateAtStep(fooRecipient, migrateStepNames.cloned);
assert.throws(function() {
    joinFooMoveChunk();
});

/**
 * Unpause bazRecipient (disable failpoint) and finish second migration, which should
 * succeed normally.
 */

jsTest.log('Finishing second migration, which should succeed....');
unpauseMigrateAtStep(bazRecipient, migrateStepNames.cloned);
joinBazMoveChunk();
assert.eq(3, bazRecipientColl.count(), 'BazRecipient does not have 3 documents.');
assert.eq(1, bazDonorColl.count(), 'Donor does not have 1 document in the baz collection.');

jsTest.log('DONE!');
st.stop();

})();