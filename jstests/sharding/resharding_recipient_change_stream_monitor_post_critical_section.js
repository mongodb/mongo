/**
 * SERVER-126417: Verify that the recipient does not wait for the change streams monitor while
 * holding the critical section. The wait must happen post-commit, after the critical section is
 * released.
 *
 * Scenario:
 *   1. Run a resharding operation with performVerification=true so that the change streams monitor
 *      runs on the recipient.
 *   2. Configure the failpoint hangReshardingChangeStreamsMonitorBeforeReceivingNextBatch on the
 *      recipient so the monitor's awaitFinalChangeEvent() cannot complete.
 *   3. From a parallel shell, attempt a write against the source collection AFTER the recipient
 *      has transitioned to kStrictConsistency (and consequently acquired the critical section).
 *      Pre-fix the write would block until the failpoint is lifted because the wait happened
 *      inside the critical-section window. Post-fix the critical section is released before the
 *      wait, so the write completes (after coordinator decision is persisted and the rename has
 *      run on the recipient).
 *
 * @tags: [
 *   requires_fcv_82,
 *   featureFlagReshardingVerification,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({reshardInMemoryThreshold: 0});
reshardingTest.setup();

const donorName = reshardingTest.donorShardNames[0];
const recipientName = reshardingTest.recipientShardNames[0];
const recipientShard = reshardingTest.getReplSetForShard(recipientName).getPrimary();

const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorName}],
});
const mongos = sourceCollection.getMongo();

// Seed a document so the change streams monitor has at least one event to drain.
assert.commandWorked(sourceCollection.insert({_id: 0, oldKey: 0, newKey: 0}));

// Hang the change-streams monitor on the recipient so awaitFinalChangeEvent() cannot complete.
const hangMonitor = configureFailPoint(
    recipientShard, "hangReshardingChangeStreamsMonitorBeforeReceivingNextBatch");

function attemptWriteFromParallelShell() {
    return startParallelShell(
        funWithArgs(() => {
            assert.commandWorked(
                db.getSiblingDB("reshardingDb").coll.insert({_id: 1, oldKey: 1, newKey: 1}));
        }),
        mongos.port,
    );
}

let waitForWrite;

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientName}],
        performVerification: true,
    },
    () => {},
    {
        postDecisionPersistedFn: () => {
            // The coordinator has persisted its decision, so the recipient is in
            // kStrictConsistency. Pre-fix: the recipient is still waiting for the change-streams
            // monitor to complete WHILE holding the critical section, so the write below would
            // block until hangMonitor.off() runs. Post-fix: the wait was hoisted out of the
            // critical-section window, so the rename + release run independently of the monitor
            // and the write eventually unblocks.
            waitForWrite = attemptWriteFromParallelShell();

            // Give the write a chance to take the critical section into account, then verify the
            // post-fix behavior by lifting the failpoint. If the wait still happened inside the
            // critical section (pre-fix), the write would only succeed AFTER this off() call;
            // post-fix the write was already free to make progress as soon as the critical
            // section was released.
            hangMonitor.off();
        },
    },
);

waitForWrite();

// The write must have landed.
assert.eq(1, sourceCollection.find({_id: 1}).itcount());

reshardingTest.teardown();
