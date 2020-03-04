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
const ns = dbName + ".foo";

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

const st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

function setUp() {
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

function tearDown() {
    st.s.getCollection(ns).drop();
}

/**
 * Generic function to run a test. 'description' is a description of the test for logging
 * purposes and 'testBody' is the test function.
 */
function test(description, testBody) {
    jsTest.log(`Running Test Setup: ${description}`);
    setUp();
    jsTest.log(`Running Test Body: ${description}`);
    testBody();
    jsTest.log(`Running Test Tear-Down: ${description}`);
    tearDown();
    jsTest.log(`Finished Running Test: ${description}`);
}

test("Refining the shard key does not prevent removal of orphaned documents", () => {
    // Enable failpoint which will cause range deletion to hang indefinitely.
    let suspendRangeDeletionFailpoint =
        configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");

    // Note that _waitForDelete has to be absent/false since we're suspending range deletion.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: shardKeyValueInChunk, to: st.shard1.shardName}));

    jsTestLog("Waiting for the suspendRangeDeletion failpoint to be hit");

    suspendRangeDeletionFailpoint.wait();

    jsTestLog("Refining the shard key");

    // Create an index on the refined shard key.
    assert.commandWorked(st.s.getCollection(ns).createIndex(refinedShardKey));

    // Refine the shard key from just the field 'x' to 'x' and 'y'.
    assert.commandWorked(st.s.adminCommand({refineCollectionShardKey: ns, key: refinedShardKey}));

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

test("Chunks with a refined shard key cannot migrate back onto a shard with " +
         "orphaned documents created with the prior shard key",
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

         jsTestLog("Attempting to move the chunk back to shard 0");
         // Attempt to move the chunk back to shard 0, sending it with maxTimeMS. Since there
         // will be orphaned documents still on shard 0 (because range deletion is paused), we
         // expected this command to time out.
         const awaitResult = startParallelShell(
             funWithArgs(function(ns, refinedShardKeyValueInChunk, toShardName) {
                 assert.commandFailedWithCode(db.adminCommand({
                     moveChunk: ns,
                     find: refinedShardKeyValueInChunk,
                     to: toShardName,
                     maxTimeMS: 1000
                 }),
                                              ErrorCodes.MaxTimeMSExpired);
             }, ns, refinedShardKeyValueInChunk, st.shard0.shardName), st.s.port);
         awaitResult();

         // Allow range deletion to continue.
         suspendRangeDeletionFailpoint.off();

         jsTestLog("Waiting for orphans to be removed from shard 0");

         // The range deletion should eventually succeed in the background.
         assert.soon(() => {
             return st.rs0.getPrimary().getCollection(ns).find().itcount() == 0;
         });

         // Moving the chunk back to shard 0 should now succeed.
         assert.commandWorked(st.s.adminCommand(
             {moveChunk: ns, find: refinedShardKeyValueInChunk, to: st.shard0.shardName}));
     });

// This test was created to reproduce a specific bug, which is why it may sound like an odd thing to
// test. See SERVER-46386 for more details.
test("Range deletion tasks created prior to refining the shard key do not " +
         "conflict with non-overlapping ranges once the shard key is refined",
     () => {
         // Enable failpoint which will cause range deletion to hang indefinitely.
         let suspendRangeDeletionFailpoint =
             configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");

         // Note that _waitForDelete has to be absent/false since we're suspending range deletion.
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
         awaitRSClientHosts(st.rs1.getPrimary(), st.rs0.getPrimary(), {ok: true, ismaster: true});

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
