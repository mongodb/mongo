/**
 * SERVER-89399: CheckShardFilteringMetadata hook must wait for in-flight resharding operations
 * to drain before sampling per-shard catalog state. Otherwise it races the resharding
 * coordinator's writes to config.collections / config.chunks and can report false positives.
 *
 * This test:
 *   1) Drives a reshardCollection to the point where the coordinator is paused right before
 *      persisting its decision (a window in which the catalog is mid-transition).
 *   2) From a parallel thread, invokes the CheckShardFilteringMetadata hook against that cluster.
 *   3) Asserts the hook does NOT return while the resharding operation is still in flight.
 *   4) Releases the coordinator failpoint, lets resharding finish, then asserts the hook
 *      returns cleanly (no false-positive metadata mismatch).
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_71,
 *   uses_atclustertime,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

const mongos = sourceCollection.getMongo();
const topology = reshardingTest.getTopology();
const configPrimary = new Mongo(topology.configsvr.nodes[0]);

// Hold the resharding coordinator right before it persists the commit decision. While paused,
// the catalog is in flight and the hook (if buggy) would observe an inconsistent snapshot.
const pauseBeforeCommit =
    configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeDecisionPersisted");

let hookThread;
let hookReleaseFp;

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    (tempNs) => {
        // Wait until the coordinator is paused on the failpoint — at this point a resharding
        // operation is unambiguously in flight.
        pauseBeforeCommit.wait();

        // Invoke the hook from a parallel thread. Capture wall-clock so we can assert the hook
        // actually blocked rather than returning instantly against a mid-flight catalog.
        hookThread = new Thread(function (mongosHost) {
            const {CheckShardFilteringMetadataHelpers} =
                await import("jstests/libs/check_shard_filtering_metadata_helpers.js");
            const conn = new Mongo(mongosHost);
            assert.commandWorked(conn.adminCommand({balancerStop: 1}));

            const shards = conn.getDB("config").shards.find().toArray();
            const startMs = Date.now();
            for (const shardDoc of shards) {
                const shardConn = new Mongo(shardDoc.host);
                shardConn.setSecondaryOk();
                CheckShardFilteringMetadataHelpers.run(conn, shardConn, shardDoc._id);
            }
            return {durationMs: Date.now() - startMs};
        }, mongos.host);
        hookThread.start();

        // Give the hook a generous window to (incorrectly) race the resharding op. A correct
        // implementation will be parked waiting for resharding to drain; a buggy one will sail
        // through and may flag a false-positive mismatch against the in-flight catalog.
        const raceWindowMs = 5 * 1000;
        sleep(raceWindowMs);

        assert(hookThread.hasOwnProperty("_thread"), "hook thread not started");
        // Tripwire: ensure hook has not yet returned. We can't directly poll Thread liveness
        // portably, so we rely on the fact that releasing the failpoint after this sleep should
        // be what unblocks the hook. If the hook had already returned, the join() below would
        // record a durationMs < raceWindowMs.
        hookReleaseFp = pauseBeforeCommit;
    },
);

// At this point withReshardingInBackground() has released the coordinator and the resharding
// operation has fully drained (the fixture waits for completion before returning).
if (hookReleaseFp) {
    // Failpoint already released by the fixture as part of teardown — guard against double-off.
    try {
        hookReleaseFp.off();
    } catch (e) {
        // already off
    }
}

assert(hookThread, "expected hook thread to have been started");
hookThread.join();
const hookResult = hookThread.returnData();

// The hook must have blocked at least until resharding drained. If it returned instantly
// (< raceWindowMs) it raced the in-flight catalog and SERVER-89399 still reproduces.
assert.gte(
    hookResult.durationMs,
    5 * 1000,
    `CheckShardFilteringMetadata hook returned in ${hookResult.durationMs}ms — expected it to ` +
        `wait for the in-flight resharding operation to drain (SERVER-89399).`,
);

reshardingTest.teardown();
