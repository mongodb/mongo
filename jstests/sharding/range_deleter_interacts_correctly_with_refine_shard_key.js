/**
 * Tests the interaction of the refineCollectionShardKey command with the range deleter.
 *
 * @tags: [requires_fcv_44]
 */
(function() {

"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');
load('jstests/replsets/rslib.js');

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

// Tests of range deletion tasks submitted in FCV 4.4.
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

    test(st,
         "Migration recovery recovers correct decision for migration committed before shard key " +
             "refine",
         () => {
             // Enable a failpoint that makes the migration donor hang before making a decision and
             // begin a migration that hits this failpoint.
             let hangBeforeWritingDecisionFailpoint =
                 configureFailPoint(st.rs0.getPrimary(), "hangBeforeMakingCommitDecisionDurable");
             const parallelMoveChunk = startParallelShell(
                 funWithArgs(function(ns, shardKeyValueInChunk, toShardName) {
                     assert.commandFailedWithCode(
                         db.adminCommand(
                             {moveChunk: ns, find: shardKeyValueInChunk, to: toShardName}),
                         ErrorCodes.InterruptedDueToReplStateChange);
                 }, ns, shardKeyValueInChunk, st.shard1.shardName), st.s.port);

             jsTestLog("Waiting for the migration to hang before writing a decision");
             hangBeforeWritingDecisionFailpoint.wait();

             // Step up a new primary, which will interrupt the migration and trigger the migration
             // recovery process. Set a failpoint on the new primary that will pause the recovery
             // before it can load the latest metadata.
             jsTestLog("Stepping up a new primary");
             const newPrimary = st.rs0.getSecondary();
             let hangInMigrationRecoveryFailpoint =
                 configureFailPoint(newPrimary, "hangBeforeFilteringMetadataRefresh");
             st.rs0.stepUp(newPrimary);
             st.rs0.waitForState(newPrimary, ReplSetTest.State.PRIMARY);

             jsTestLog("Waiting for the new primary to hang in migration recovery");
             hangInMigrationRecoveryFailpoint.wait();

             // Clean up the failpoint on the old primary.
             hangBeforeWritingDecisionFailpoint.off();

             // Wait for relevant nodes to detect the new primary, which may take some time using
             // the RSM protocols other than streamable.
             awaitRSClientHosts(st.s, newPrimary, {ok: true, ismaster: true});
             awaitRSClientHosts(st.rs1.getPrimary(), newPrimary, {ok: true, ismaster: true});
             awaitRSClientHosts(st.configRS.getPrimary(), newPrimary, {ok: true, ismaster: true});

             // Refine the collection's shard key while the recovery task is hung.
             jsTestLog("Refining the shard key");
             assert.commandWorked(st.s.getCollection(ns).createIndex({x: 1, y: 1, z: 1}));
             assert.commandWorked(
                 st.s.adminCommand({refineCollectionShardKey: ns, key: {x: 1, y: 1, z: 1}}));

             // Allow the recovery to continue by disabling the failpoint and verify that despite
             // the recovered migration having fewer fields in its bounds than in the current shard
             // key, the decision should be recovered successfully and orphans should be removed
             // from the donor.
             jsTestLog("Waiting for orphans to be removed from shard 0");
             hangInMigrationRecoveryFailpoint.off();
             assert.soon(() => {
                 return st.rs0.getPrimary().getCollection(ns).find().itcount() == 0;
             });

             // Verify we can move the chunk back to the original donor once the orphans are gone.
             assert.commandWorked(st.s.adminCommand({
                 moveChunk: ns,
                 find: {x: 1, y: 1, z: 1},
                 to: st.shard0.shardName,
                 _waitForDelete: true
             }));
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

// Tests of range deletion tasks submitted in FCV 4.2.
(() => {
    const st = new ShardingTest({shards: 2});

    test(st,
         "Refining the shard key does not prevent removal of orphaned documents on a donor" +
             " shard after a successful migration",
         () => {
             assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

             // Enable failpoint which will cause range deletion to hang indefinitely.
             let suspendRangeDeletionFailpoint =
                 configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");

             // Note that _waitForDelete has to be absent/false since we're suspending range
             // deletion.
             assert.commandWorked(st.s.adminCommand(
                 {moveChunk: ns, find: shardKeyValueInChunk, to: st.shard1.shardName}));

             jsTestLog("Waiting for the suspendRangeDeletion failpoint to be hit");
             suspendRangeDeletionFailpoint.wait();

             assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.4"}));

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
             assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

             // Enable failpoint which will cause range deletion to hang indefinitely.
             let suspendRangeDeletionFailpoint =
                 configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");

             // Note that _waitForDelete has to be absent/false since we're suspending range
             // deletion.
             assert.commandWorked(st.s.adminCommand(
                 {moveChunk: ns, find: shardKeyValueInChunk, to: st.shard1.shardName}));

             jsTestLog("Waiting for the suspendRangeDeletion failpoint to be hit");
             suspendRangeDeletionFailpoint.wait();

             assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.4"}));

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
             " shard on which the shard key index was never built",
         () => {
             // Fail a migration of a chunk from shard 0 to shard 1 so that shard 1 creates the
             // collection on itself with the correct UUID.
             let migrationCommitVersionError =
                 configureFailPoint(st.configRS.getPrimary(), "migrationCommitVersionError");
             assert.commandFailedWithCode(st.s.adminCommand({
                 moveChunk: ns,
                 find: shardKeyValueInChunk,
                 to: st.shard1.shardName,
             }),
                                          ErrorCodes.StaleEpoch);
             migrationCommitVersionError.off();

             assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.4"}));

             jsTestLog("Refining the shard key");

             // Create an index on the refined shard key.
             assert.commandWorked(st.s.getCollection(ns).createIndex(refinedShardKey));

             // Refine the shard key from just the field 'x' to 'x' and 'y'.
             assert.commandWorked(
                 st.s.adminCommand({refineCollectionShardKey: ns, key: refinedShardKey}));

             // The index on the original shard key shouldn't be required anymore.
             assert.commandWorked(st.s.getCollection(ns).dropIndex(originalShardKey));

             assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

             // Ensure it's possible to move a chunk from shard 0 to shard 1, though shard 1 did not
             // own any chunks when the refined shard key index was created.
             // TODO (SERVER-47025): Without creating this index, the range deleter will hang
             // indefinitely looking for a shard key index.
             assert.commandWorked(st.shard1.getCollection(ns).createIndex(refinedShardKey));
             assert.commandWorked(st.s.adminCommand(
                 {moveChunk: ns, find: refinedShardKeyValueInChunk, to: st.shard1.shardName}));
         });

    st.stop();
})();
})();
