/**
 * Tests the interaction of the refineCollectionShardKey command with the range deleter.
 *
 * @tags: [requires_fcv_44]
 */
(function() {

"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

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

// Tests with resumable range deleter enabled.
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

             // We will use this to wait until the following migration has completed, since we
             // expect the client to time out. Waiting for this failpoint technically just waits for
             // the recipient side of the migration to complete, but it's expected that if the
             // migration can get to that point, then it should be able to succeed overall.
             let hangDonorAtEndOfMigration =
                 configureFailPoint(st.rs1.getPrimary(), "moveChunkHangAtStep6");

             jsTestLog("Attempting to move the chunk back to shard 0");
             // Attempt to move the chunk back to shard 0, sending it with maxTimeMS. Since there
             // will be orphaned documents still on shard 0 (because range deletion is paused), we
             // expected this command to time out.
             assert.commandFailedWithCode(st.s.adminCommand({
                 moveChunk: ns,
                 find: refinedShardKeyValueInChunk,
                 to: st.shard0.shardName,
                 maxTimeMS: 1000
             }),
                                          ErrorCodes.MaxTimeMSExpired);

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

             // Wait for the donor to learn about the new primary on the recipient.
             awaitRSClientHosts(
                 st.rs1.getPrimary(), st.rs0.getPrimary(), {ok: true, ismaster: true});

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

// Tests with resumable range deleter disabled.
(() => {
    const st = new ShardingTest(
        {shards: 2, shardOptions: {setParameter: {"disableResumableRangeDeleter": true}}});

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

             // We will use this to wait until the following migration has completed, since we
             // expect the client to time out. Waiting for this failpoint technically just waits for
             // the recipient side of the migration to complete, but it's expected that if the
             // migration can get to that point, then it should be able to succeed overall.
             let hangDonorAtEndOfMigration =
                 configureFailPoint(st.rs1.getPrimary(), "moveChunkHangAtStep6");

             jsTestLog("Attempting to move the chunk back to shard 0");

             // Attempt to move the chunk back to shard 0, sending it with maxTimeMS. Since there
             // will be orphaned documents still on shard 0 (because range deletion is paused), we
             // expected this command to time out. This will NOT fail the migration, however, since
             // that occurs in a background OperationContext.
             assert.commandFailedWithCode(st.s.adminCommand({
                 moveChunk: ns,
                 find: refinedShardKeyValueInChunk,
                 to: st.shard0.shardName,
                 maxTimeMS: 1000
             }),
                                          ErrorCodes.MaxTimeMSExpired);

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

             // TODO (SERVER-47003): There will be a left-over entry in config.migrations after the
             // previous moveChunk fails with MaxTimeMSExpired, so we drop the collection. Otherwise
             // future migrations would receive a DuplicateKeyError when trying to update
             // config.migrations.
             st.config.getSiblingDB('config').migrations.drop();
         });

    test(st,
         "Refining the shard key does not prevent removal of orphaned documents on a recipient" +
             " shard after a failed migration",
         () => {
             let hangRecipientAfterCloningDocuments =
                 configureFailPoint(st.rs1.getPrimary(), "migrateThreadHangAtStep3");

             // Attempt to move the chunk to shard 1. This will clone all documents from shard 0 to
             // shard 1 and then block behind the hangRecipientAfterCloningDocuments failpoint.
             // Then, when the index is created on the refined shard key, the migration will be
             // interrupted, causing it to fail with error code Interrupted.
             const awaitResult =
                 startParallelShell(funWithArgs(function(ns, shardKeyValueInChunk, toShardName) {
                                        assert.commandFailedWithCode(db.adminCommand({
                                            moveChunk: ns,
                                            find: shardKeyValueInChunk,
                                            to: toShardName,
                                            _waitForDelete: true
                                        }),
                                                                     ErrorCodes.Interrupted);
                                        jsTestLog("Recipient failed in parallel shell");
                                    }, ns, shardKeyValueInChunk, st.shard1.shardName), st.s.port);

             jsTestLog("Waiting for recipient to finish cloning documents");

             hangRecipientAfterCloningDocuments.wait();

             jsTestLog("Refining the shard key");

             // Create an index on the refined shard key.
             assert.commandWorked(st.s.getCollection(ns).createIndex(refinedShardKey));

             // Refine the shard key from just the field 'x' to 'x' and 'y'.
             assert.commandWorked(
                 st.s.adminCommand({refineCollectionShardKey: ns, key: refinedShardKey}));

             // The index on the original shard key shouldn't be required anymore.
             assert.commandWorked(st.s.getCollection(ns).dropIndex(originalShardKey));

             // Turn off failpoint and wait for recipient to fail.
             hangRecipientAfterCloningDocuments.off();
             awaitResult();

             // TODO (SERVER-47025): Without creating this index, the range deleter will hang
             // indefinitely looking for a shard key index.
             assert.commandWorked(st.shard1.getCollection(ns).createIndex(refinedShardKey));

             jsTestLog("Waiting for orphans to be removed from shard 1");

             // The range deletion should eventually succeed in the background on the recipient.
             assert.soon(() => {
                 return st.rs1.getPrimary().getCollection(ns).find().itcount() == 0;
             });
         });

    st.stop();
})();
})();
