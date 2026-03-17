/**
 * Tests that the resharding oplog application phase increments the non-deprioritizable operations
 * counter on both donor and recipient shards. During the applying phase, the oplog fetcher's
 * aggregate on the donor's oplog.rs (donor-side) and the oplog fetcher's batch processing
 * (recipient-side) are both marked non-deprioritizable to prevent execution control from starving
 * critical resharding work.
 *
 * @tags: [
 *   uses_atclustertime,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";
import {getTotalMarkedNonDeprioritizableCount} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

const reshardingTest = new ReshardingTest({numDonors: 1, numRecipients: 1, reshardInPlace: false});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;
const ns = jsTestName() + ".coll";

const sourceCollection = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

const bulkOp = sourceCollection.initializeUnorderedBulkOp();
for (let i = 0; i < 10; i++) {
    bulkOp.insert({oldKey: i, newKey: i});
}
assert.commandWorked(bulkOp.execute());

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const donorPrimary = new Mongo(topology.shards[donorShardNames[0]].primary);
const recipientPrimary = new Mongo(topology.shards[recipientShardNames[0]].primary);
const configPrimary = new Mongo(topology.configsvr.primary);

const pauseBeforeApplying = configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeApplying");

let donorCountBefore;
let recipientCountBefore;

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        pauseBeforeApplying.wait();

        donorCountBefore = getTotalMarkedNonDeprioritizableCount(donorPrimary);
        recipientCountBefore = getTotalMarkedNonDeprioritizableCount(recipientPrimary);
        jsTestLog(`Counters before applying - donor: ${donorCountBefore}, ` + `recipient: ${recipientCountBefore}`);

        // Insert documents after the clone timestamp. These become oplog entries that the
        // recipient's oplog fetcher must retrieve from the donor and apply during the applying
        // phase, guaranteeing non-deprioritizable work on both sides.
        for (let i = 100; i < 120; i++) {
            assert.commandWorked(sourceCollection.insert({oldKey: i, newKey: i}));
        }

        pauseBeforeApplying.off();
    },
    {
        postCheckConsistencyFn: () => {
            const donorCountAfter = getTotalMarkedNonDeprioritizableCount(donorPrimary);
            const recipientCountAfter = getTotalMarkedNonDeprioritizableCount(recipientPrimary);
            jsTestLog(`Counters after applying - donor: ${donorCountAfter}, ` + `recipient: ${recipientCountAfter}`);

            assert.gt(
                donorCountAfter,
                donorCountBefore,
                "Donor non-deprioritizable counter should increase during the applying phase",
            );
            assert.gt(
                recipientCountAfter,
                recipientCountBefore,
                "Recipient non-deprioritizable counter should increase during the applying phase",
            );
        },
    },
);

reshardingTest.teardown();
