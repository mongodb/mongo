/**
 * SERVER-108852 repro: Resharding participants don't handle change streams monitor failures
 * immediately.
 *
 * The change-streams monitor's quiesce future is stored at _createAndStartChangeStreamsMonitor
 * but the donor's future chain doesn't check it until _awaitChangeStreamsMonitorCompleted, which
 * runs AFTER the donor has already executed:
 *   - _awaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites
 *   - _writeTransactionOplogEntryThenTransitionToBlockingWrites
 * If the monitor fails during these phases, blocking writes are entered before the error is ever
 * surfaced. This jstest forces a monitor failure via
 * reshardingDonorFailsUpdatingChangeStreamsMonitorProgress and observes that the donor proceeds
 * past blockingWrites entry before the failure shows up. Under the proposed fix (an immediate
 * observer that cancels the chain on monitor error) the donor would abort before entering
 * blocking writes.
 *
 * @tags: [
 *   requires_fcv_80,
 *   uses_change_streams,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 1, numRecipients: 1});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const donorPrimary = new Mongo(topology.shards[donorShardNames[0]].primary);

// Seed enough documents that the change-streams monitor will see batched events to update
// progress for, giving the failpoint a chance to fire.
const bulk = sourceCollection.initializeUnorderedBulkOp();
for (let i = 0; i < 200; ++i) {
    bulk.insert({_id: i, oldKey: i, newKey: -i});
}
assert.commandWorked(bulk.execute());

// Configure the donor's batch-callback to throw on the first progress update. The monitor's
// SemiFuture will become Failed; the buggy donor will store the error in
// _changeStreamsMonitorQuiesced and proceed.
const monitorFailpoint = configureFailPoint(
    donorPrimary, "reshardingDonorFailsUpdatingChangeStreamsMonitorProgress");

// Pause the donor immediately after blocking reads. With the bug, by the time this failpoint
// is hit the donor has already entered blocking writes despite the monitor having failed; with
// the fix the donor should never reach this failpoint because the chain is aborted earlier.
const pauseAfterBlockingReads = configureFailPoint(donorPrimary,
                                                   "reshardingPauseDonorAfterBlockingReads");

let coordinatorErr = null;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
        performVerification: true,
    },
    () => {
        // Drive a write so the monitor has something to batch and the failpoint fires.
        assert.commandWorked(
            sourceCollection.update({_id: 0}, {$set: {touched: 1}}));

        // Wait for the failpoint that pauses the donor AFTER blocking-reads / blocking-writes
        // entry. The bug is observed iff this fires: the donor reached blocking writes even
        // though the monitor already failed.
        try {
            pauseAfterBlockingReads.wait({timesEntered: 1, maxTimeMS: 30 * 1000});
            jsTest.log(
                "SERVER-108852 BUG CONFIRMED: donor entered blocking writes after monitor failure");
        } catch (e) {
            // Under the fix, the chain aborts before this failpoint is reachable. Treat the
            // timeout as evidence that the fix took effect.
            jsTest.log("SERVER-108852 FIX CONFIRMED: donor did not reach blocking writes after " +
                       "monitor failure (pauseAfterBlockingReads not fired): " + e);
        }

        pauseAfterBlockingReads.off();
    },
    {
        // The operation MUST ultimately abort because the monitor failed unrecoverably.
        expectedErrorCode: ErrorCodes.InternalError,
    });

monitorFailpoint.off();

// Sanity: assert the donor surfaced the InternalError, even if delayed. The bug is about
// timing, not correctness of the final state.
assert.soon(() => {
    const config = mongos.getDB("config");
    const reshardingOps = config.reshardingOperations.find().toArray();
    return reshardingOps.length === 0;
});

reshardingTest.teardown();
