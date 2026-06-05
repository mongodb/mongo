/**
 * Tests that:
 * - The resharding cloner gets deprioritized both on donor and recipient shards.
 * - The resharding oplog application phase increments the non-deprioritizable operations
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
import {
    getTotalDeprioritizationCount,
    getTotalMarkedNonDeprioritizableCount,
} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

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
const donorNodes = topology.shards[donorShardNames[0]].nodes.map((host) => new Mongo(host));
const recipientNodes = topology.shards[recipientShardNames[0]].nodes.map((host) => new Mongo(host));
const configPrimary = new Mongo(topology.configsvr.primary);

function sumDonorsTotalDeprioritizationCount() {
    // Summing metrics from all donor nodes beause cloning happens from the nearest node
    return donorNodes.reduce((sum, node) => sum + getTotalDeprioritizationCount(node), 0);
}

for (const node of donorNodes) {
    assert.commandWorked(node.adminCommand({setParameter: 1, executionControlDeprioritizationGate: true}));
}
for (const node of recipientNodes) {
    assert.commandWorked(node.adminCommand({setParameter: 1, executionControlDeprioritizationGate: true}));
}

const pauseBeforeCloning = configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeCloning");
const pauseBeforeApplying = configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeApplying");

let nonDeprioritizableDonorCountBeforeCatchup;
let nonDeprioritizableRecipientCountBeforeCatchup;

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        pauseBeforeCloning.wait();
        const deprioritizationDonorCountBeforeCloning = sumDonorsTotalDeprioritizationCount();
        const deprioritizationRecipientCountBeforeCloning = getTotalDeprioritizationCount(recipientPrimary);
        pauseBeforeCloning.off();

        pauseBeforeApplying.wait();
        const deprioritizationDonorCountAfterCloning = sumDonorsTotalDeprioritizationCount();
        const deprioritizationRecipientCountAfterCloning = getTotalDeprioritizationCount(recipientPrimary);

        assert.gt(
            deprioritizationDonorCountAfterCloning,
            deprioritizationDonorCountBeforeCloning,
            "Donor deprioritizable counter should increase during the cloning phase",
        );
        assert.gt(
            deprioritizationRecipientCountAfterCloning,
            deprioritizationRecipientCountBeforeCloning,
            "Recipient deprioritizable counter should increase during the cloning phase",
        );

        nonDeprioritizableDonorCountBeforeCatchup = getTotalMarkedNonDeprioritizableCount(donorPrimary);
        nonDeprioritizableRecipientCountBeforeCatchup = getTotalMarkedNonDeprioritizableCount(recipientPrimary);
        jsTestLog(
            `Counters before applying - donor: ${nonDeprioritizableDonorCountBeforeCatchup}, ` +
                `recipient: ${nonDeprioritizableRecipientCountBeforeCatchup}`,
        );

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
            const nonDeprioritizableDonorCountAfterCatchup = getTotalMarkedNonDeprioritizableCount(donorPrimary);
            const nonDeprioritizableRecipientCountAfterCatchup =
                getTotalMarkedNonDeprioritizableCount(recipientPrimary);
            jsTestLog(
                `Counters after applying - donor: ${nonDeprioritizableDonorCountAfterCatchup}, ` +
                    `recipient: ${nonDeprioritizableRecipientCountAfterCatchup}`,
            );

            assert.gt(
                nonDeprioritizableDonorCountAfterCatchup,
                nonDeprioritizableDonorCountBeforeCatchup,
                "Donor non-deprioritizable counter should increase during the applying phase",
            );
            assert.gt(
                nonDeprioritizableRecipientCountAfterCatchup,
                nonDeprioritizableRecipientCountBeforeCatchup,
                "Recipient non-deprioritizable counter should increase during the applying phase",
            );
        },
    },
);

reshardingTest.teardown();
