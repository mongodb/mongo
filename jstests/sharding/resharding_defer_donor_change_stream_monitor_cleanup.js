/**
 * Tests that the donor's change streams monitor is NOT torn down as soon as the donor finishes
 * its local work. Its cleanup must be deferred until the coordinator has persisted the commit
 * decision and explicitly instructed the donor to wind the monitor down, so that the documents-
 * delta the monitor accumulates remains available for post-commit oplog count verification.
 *
 * SERVER-126444 (parent SERVER-125429: "Complete the oplog count verification post commit").
 *
 * @tags: [
 *   requires_fcv_90,
 *   featureFlagReshardingVerification,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 1});
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
const topology = DiscoverTopology.findConnectedNodes(mongos);

const donor0 = new Mongo(topology.shards[donorShardNames[0]].primary);
const donor1 = new Mongo(topology.shards[donorShardNames[1]].primary);

// Sit the monitor on each donor right before it would kill its change-stream cursor as part of
// cleanup. While these fail points are armed, the monitor cannot be torn down even if the donor
// state machine asks for cleanup early -- so if the deferral logic is correct, the monitor will
// remain alive past commit and will only quiesce once the coordinator instruction unblocks the
// fail points.
const hangDonor0BeforeKill = configureFailPoint(
    donor0,
    "hangReshardingChangeStreamsMonitorBeforeKillingCursors",
);
const hangDonor1BeforeKill = configureFailPoint(
    donor1,
    "hangReshardingChangeStreamsMonitorBeforeKillingCursors",
);

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]},
        ],
        performVerification: true,
    },
    () => {
        // Generate write traffic across both donors after cloneTimestamp so the change streams
        // monitor has a non-trivial documents-delta to carry into post-commit verification.
        reshardingTest.awaitCloneTimestampChosen();
        const coll = sourceCollection;
        for (let i = 1; i <= 25; i++) {
            assert.commandWorked(coll.insert({_id: -i, oldKey: -i, newKey: -i}));
            assert.commandWorked(coll.insert({_id: i, oldKey: i, newKey: i}));
        }
        assert.commandWorked(coll.deleteOne({_id: -1}));
        assert.commandWorked(coll.updateOne({_id: 1}, {$set: {touched: true}}));
    },
    {
        // Runs after the coordinator has persisted its commit decision but before the operation
        // returns. If donor cleanup were *not* deferred, the monitor would already have been
        // killed by this point and the fail points (which only fire from inside the monitor's
        // shutdown path) would have zero hits. The invariant under test is the opposite: every
        // donor's monitor is still in its cleanup path, parked on the fail point, waiting for
        // the coordinator to wave it through.
        postDecisionPersistedFn: () => {
            assert.soon(
                () => {
                    const h0 = assert.commandWorked(donor0.adminCommand({
                        configureFailPoint: "hangReshardingChangeStreamsMonitorBeforeKillingCursors",
                        mode: "alwaysOn",
                    }));
                    const h1 = assert.commandWorked(donor1.adminCommand({
                        configureFailPoint: "hangReshardingChangeStreamsMonitorBeforeKillingCursors",
                        mode: "alwaysOn",
                    }));
                    return h0.count >= 1 && h1.count >= 1;
                },
                "donor change streams monitor was cleaned up before the coordinator instructed it" +
                    " to -- the deferral invariant from SERVER-126444 is violated",
                30 * 1000,
            );

            // Release the fail points so the monitor can complete cleanup once the coordinator
            // actually instructs it. The resharding operation must still return success.
            hangDonor0BeforeKill.off();
            hangDonor1BeforeKill.off();
        },
    },
);

reshardingTest.teardown();
