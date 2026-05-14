/**
 * SERVER-126445: The donor critical section must not hold open while waiting for the change
 * streams monitor to drain. The wait has been moved out of the
 * _runUntilBlockingWritesOrErrored chain (which runs while writes are blocked) and into
 * _finishReshardingOperation, AFTER releaseRecoverableCriticalSection has been called.
 *
 * This test pins the new ordering by:
 *  1) Enabling resharding verification so the change streams monitor is created.
 *  2) Hanging the donor's change streams monitor mid-flight via the
 *     hangReshardingChangeStreamsMonitorBeforeReceivingNextBatch failpoint.
 *  3) Letting the resharding operation reach the donor's blocking-writes state.
 *  4) Asserting that the donor transitions PAST blocking-writes (i.e. its critical section
 *     is released and writes are unblocked) WHILE the monitor is still hung. Pre-fix the
 *     donor would remain stuck at donorState=blocking-writes until the monitor drained.
 *  5) Lifting the failpoint and letting the operation complete normally.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
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

// Seed a couple of documents so the change streams monitor sees real events to process.
assert.commandWorked(sourceCollection.insert([{_id: 0, oldKey: 0, newKey: 0}, {_id: 1, oldKey: 1, newKey: 1}]));

function getDonorState() {
    const ops = donorPrimary
        .getDB("admin")
        .aggregate([
            {$currentOp: {allUsers: true, localOps: false}},
            {$match: {type: "op", desc: /ReshardingDonorService/, donorState: {$exists: true}}},
        ])
        .toArray();
    if (ops.length === 0) {
        return undefined;
    }
    return ops[0].donorState;
}

// Hang the donor's change streams monitor between batches. With the SERVER-126445 fix, the
// donor must still be able to release its critical section and advance past blocking-writes
// while this failpoint is engaged. Pre-fix the wait happened inside the critical section, so
// the donor would block here indefinitely.
const monitorHang = configureFailPoint(donorPrimary, "hangReshardingChangeStreamsMonitorBeforeReceivingNextBatch");

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
        performVerification: true,
    },
    () => {
        // Wait until the donor reaches blocking-writes. This is the moment the critical section
        // has been acquired and the bug-fix is observable: pre-fix the donor would now block on
        // the change streams monitor; post-fix it must move on.
        assert.soon(
            () => getDonorState() === "blocking-writes" || getDonorState() === "done",
            () => `Donor never reached blocking-writes. Current state: ${getDonorState()}`,
        );

        // Wait for the monitor failpoint to actually trip — this proves the monitor is running
        // and is being held back. If we proceeded without this, a fast cluster could drain the
        // monitor's only batch before the failpoint engaged, making the test vacuous.
        monitorHang.wait();

        // The load-bearing assertion: with the monitor still hung, the donor must transition
        // past blocking-writes. Pre-fix this assertion times out because the donor is parked
        // inside the critical section waiting for the monitor.
        assert.soon(
            () => {
                const s = getDonorState();
                return s === "done" || s === undefined; // undefined => donor doc already removed
            },
            () =>
                `Donor stuck at donorState=${getDonorState()} while change streams monitor is hung. ` +
                `Pre-SERVER-126445 the donor would wait for the monitor inside the critical section.`,
            5 * 60 * 1000,
            1000,
        );

        // Lift the failpoint so the monitor can drain and the resharding operation can finish.
        monitorHang.off();
    },
);

reshardingTest.teardown();
