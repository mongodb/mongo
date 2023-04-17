/**
 * Tests the interaction of the refineCollectionShardKey command with the range deleter.
 */

// Cannot run the filtering metadata check on tests that run refineCollectionShardKey.
TestData.skipCheckShardFilteringMetadata = true;

(function() {

"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');
load('jstests/replsets/rslib.js');
load('jstests/libs/feature_flag_util.js');

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const originalShardKey = {
    x: 1
};

const refinedShardKey = {
    x: 1,
    y: 1
};

const shardKeyValueInChunk = {
    x: 1
};

const shardKeyValueInFirstChunk = {
    x: -2
};

const refinedShardKeyValueInChunk = {
    x: 1,
    y: 1
};

function setUp(st) {
    // Create a sharded collection with two chunk on shard0, split at key {x: -1}.
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: originalShardKey}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: -1}}));
    // Insert documents into the collection, which contains two chunks. Insert documents only
    // into the second chunk
    for (let i = 0; i < 100; i++) {
        st.s.getCollection(ns).insert({x: i});
    }
}

function tearDown(st) {
    st.s.getCollection(ns).drop();
}

/**
 * Generic function to run a test. 'description' is a description of the test for logging
 * purposes and 'testBody' is the test function.
 */
function test(st, description, testBody) {
    jsTest.log(`Running Test Setup: ${description}`);
    setUp(st);
    jsTest.log(`Running Test Body: ${description}`);
    testBody();
    jsTest.log(`Running Test Tear-Down: ${description}`);
    tearDown(st);
    jsTest.log(`Finished Running Test: ${description}`);
}

(() => {
    const st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});
    test(st,
         "Refining the shard key does not prevent removal of orphaned documents on a donor" +
             " shard after a successful migration",
         () => {
             // Enable failpoint which will cause range deletion to hang indefinitely.
             let suspendRangeDeletionFailpoint =
                 configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");

             // Note that _waitForDelete has to be absent/false since we're suspending range
             // deletion.
             assert.commandWorked(st.s.adminCommand(
                 {moveChunk: ns, find: shardKeyValueInChunk, to: st.shard1.shardName}));

             jsTestLog("Waiting for the suspendRangeDeletion failpoint to be hit");

             suspendRangeDeletionFailpoint.wait();

             jsTestLog("Refining the shard key");

             // Create an index on the refined shard key.
             assert.commandWorked(st.s.getCollection(ns).createIndex(refinedShardKey));

             // Refine the shard key from just the field 'x' to 'x' and 'y'.
             assert.commandWorked(
                 st.s.adminCommand({refineCollectionShardKey: ns, key: refinedShardKey}));

             // The index on the original shard key shouldn't be required anymore.
             assert.commandWorked(st.s.getCollection(ns).dropIndex(originalShardKey));

             // Allow range deletion to continue.
             suspendRangeDeletionFailpoint.off();

             jsTestLog("Waiting for orphans to be removed from shard 0");

             // The range deletion should eventually succeed in the background.
             assert.soon(() => {
                 return st.rs0.getPrimary().getCollection(ns).find().itcount() == 0;
             });
         });

    test(st,
         "Chunks with a refined shard key cannot migrate back onto a shard with " +
             "an orphaned range created with the prior shard key",
         () => {
             // Enable failpoint which will cause range deletion to hang indefinitely.
             let suspendRangeDeletionFailpoint =
                 configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");

             // Note that _waitForDelete has to be absent/false since we're suspending range
             // deletion.
             assert.commandWorked(st.s.adminCommand(
                 {moveChunk: ns, find: shardKeyValueInChunk, to: st.shard1.shardName}));

             jsTestLog("Waiting for the suspendRangeDeletion failpoint to be hit");

             suspendRangeDeletionFailpoint.wait();

             jsTestLog("Refining the shard key");

             // Create an index on the refined shard key.
             assert.commandWorked(st.s.getCollection(ns).createIndex(refinedShardKey));

             // Refine the shard key from just the field 'x' to 'x' and 'y'.
             assert.commandWorked(
                 st.s.adminCommand({refineCollectionShardKey: ns, key: refinedShardKey}));

             // The index on the original shard key shouldn't be required anymore.
             assert.commandWorked(st.s.getCollection(ns).dropIndex(originalShardKey));

             // We will use this to wait until the following migration has completed. Waiting for
             // this failpoint technically just waits for the recipient side of the migration to
             // complete, but it's expected that if the migration can get to that point, then it
             // should be able to succeed overall.
             let hangDonorAtEndOfMigration =
                 configureFailPoint(st.rs1.getPrimary(), "moveChunkHangAtStep6");

             // Attempt to move the chunk back to shard 0. Synchronize with the parallel shell to
             // make sure that the moveChunk started.
             let hangOnStep1 = configureFailPoint(st.rs1.getPrimary(), "moveChunkHangAtStep1");
             const awaitResult = startParallelShell(
                 funWithArgs(function(ns, toShardName, middle) {
                     jsTestLog("Attempting to move the chunk back to shard 0");
                     assert.commandWorked(
                         db.adminCommand({moveChunk: ns, find: middle, to: toShardName}));
                 }, ns, st.shard0.shardName, refinedShardKeyValueInChunk), st.s.port);

             hangOnStep1.wait();
             hangOnStep1.off();

             // Hang after waiting for orphan cleanup so that in the test we can check for orphans
             // on disk before documents begin migrating.
             let hangRecipient =
                 configureFailPoint(st.rs0.getPrimary(), "migrateThreadHangAtStep1");

             // Allow range deletion to continue.
             suspendRangeDeletionFailpoint.off();

             jsTestLog("Waiting for orphans to be removed from shard 0");

             // The range deletion should eventually succeed in the background.
             assert.soon(() => {
                 return st.rs0.getPrimary().getCollection(ns).find().itcount() == 0;
             });

             hangRecipient.off();

             // Wait for the previous migration to complete before continuing.
             hangDonorAtEndOfMigration.wait();
             hangDonorAtEndOfMigration.off();
             awaitResult();
         });

    // This test was created to reproduce a specific bug, which is why it may sound like an odd
    // thing to test. See SERVER-46386 for more details.
    test(st,
         "Range deletion tasks created prior to refining the shard key do not " +
             "conflict with non-overlapping ranges once the shard key is refined",
         () => {
             // Enable failpoint which will cause range deletion to hang indefinitely.
             let suspendRangeDeletionFailpoint =
                 configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");

             // Note that _waitForDelete has to be absent/false since we're suspending range
             // deletion.
             assert.commandWorked(st.s.adminCommand(
                 {moveChunk: ns, find: shardKeyValueInChunk, to: st.shard1.shardName}));

             jsTestLog("Waiting for the suspendRangeDeletion failpoint to be hit");

             suspendRangeDeletionFailpoint.wait();

             jsTestLog("Refining the shard key");

             // Create an index on the refined shard key.
             assert.commandWorked(st.s.getCollection(ns).createIndex(refinedShardKey));

             // Refine the shard key from just the field 'x' to 'x' and 'y'.
             assert.commandWorked(
                 st.s.adminCommand({refineCollectionShardKey: ns, key: refinedShardKey}));

             // The index on the original shard key shouldn't be required anymore.
             assert.commandWorked(st.s.getCollection(ns).dropIndex(originalShardKey));

             // Step down current primary.
             assert.commandWorked(st.rs0.getPrimary().adminCommand(
                 {replSetStepDown: ReplSetTest.kForeverSecs, force: 1}));

             // Allow range deletion to continue on old node. This isn't required for this test to
             // proceed since we only care about the new primary, but it's worth cleaning up.
             suspendRangeDeletionFailpoint.off();

             jsTestLog("Waiting for orphans to be removed from shard 0");

             // The range deletion should eventually succeed in the background on the new primary.
             assert.soon(() => {
                 return st.rs0.getPrimary().getCollection(ns).find().itcount() == 0;
             });

             jsTestLog("Waiting for relevant nodes to see new rs0 primary");
             // Wait for relevant nodes to detect the new primary, which may take some time using
             // the RSM protocols other than streamable.
             let newPrimary = st.rs0.getPrimary();
             awaitRSClientHosts(st.s, newPrimary, {ok: true, ismaster: true});
             awaitRSClientHosts(st.rs1.getPrimary(), newPrimary, {ok: true, ismaster: true});
             awaitRSClientHosts(st.configRS.getPrimary(), newPrimary, {ok: true, ismaster: true});

             // We should be able to move the chunk back to shard 0 now that orphans are gone.
             assert.commandWorked(st.s.adminCommand({
                 moveChunk: ns,
                 find: refinedShardKeyValueInChunk,
                 to: st.shard0.shardName,
                 _waitForDelete: true
             }));
         });

    st.stop();
})();
})();
