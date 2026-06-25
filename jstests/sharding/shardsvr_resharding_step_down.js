/**
 * Tests that _shardsvrReshardingStepDown can successfully be executed against the config
 * server. We test case where it is a single shard config shard (config server is all roles:
 * coordinator, donor, recipient) and the case with dedicated config server.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

function runStepDownTest(useConfigShard) {
    const reshardingTest = new ReshardingTest({
        configShard: useConfigShard,
        reshardInPlace: true,
    });
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const sourceCollection = reshardingTest.createShardedCollection({
        ns: "reshardingDb.coll",
        shardKeyPattern: {oldKey: 1},
        chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
    });

    assert.commandWorked(
        sourceCollection.insert([
            {oldKey: 1, newKey: 1},
            {oldKey: 2, newKey: 2},
            {oldKey: 3, newKey: 3},
        ]),
    );

    const recipientShardNames = reshardingTest.recipientShardNames;

    // In config shard + reshardInPlace mode, coordinator, donor, and recipient services all run on
    // the config shard primary. _shardsvrReshardingStepDown on that node exercises all three.
    const configShardPrimary = reshardingTest
        .getReplSetForShard(reshardingTest.configShardName)
        .getPrimary();

    let fp = configureFailPoint(configShardPrimary, "reshardingPauseCoordinatorBeforeApplying");

    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {newKey: 1},
            newChunks: [
                {min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]},
            ],
        },
        () => {
            fp.wait();
            assert.commandWorked(configShardPrimary.adminCommand({_shardsvrReshardingStepDown: 1}));

            // Turn off the failpoint so rebuilt instances don't block at the same stage.
            fp.off();
        },
    );

    reshardingTest.teardown();
}

describe("_shardsvrReshardingStepDown", function () {
    it("runs the command against config with config shard setup", function () {
        runStepDownTest(true);
    });

    // This is the case where we will have empty donor and recipient instances.
    it("runs the command against config with dedicated config setup", function () {
        runStepDownTest(false);
    });
});
