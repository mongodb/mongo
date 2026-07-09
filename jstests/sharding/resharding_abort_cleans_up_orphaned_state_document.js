/**
 * Tests that aborting a resharding operation cleans up a recipient state document that was orphaned
 * because the participant failed to construct its in-memory state machine. See SERVER-130696.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const sourceNs = "reshardingDb.coll";

const reshardingTest = new ReshardingTest();
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const inputCollection = reshardingTest.createShardedCollection({
    ns: sourceNs,
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

const mongos = inputCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const recipientPrimary = new Mongo(topology.shards[recipientShardNames[0]].primary);

// Fail the recipient with an unrecoverable error after it inserts its state document but before it
// constructs the in-memory state machine, orphaning the state document.
const failpoint = configureFailPoint(
    recipientPrimary,
    "reshardingInterruptAfterInsertStateMachineDocument",
    {errorCode: ErrorCodes.InternalError},
);

const recipientStateDocs = recipientPrimary.getCollection(
    "config.localReshardingOperations.recipient",
);

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        failpoint.wait();
    },
    {expectedErrorCode: ErrorCodes.InternalError},
);

failpoint.off();

// The orphaned recipient state document must be cleaned up as part of the abort.
assert.soon(
    () => recipientStateDocs.findOne() === null,
    "recipient state document was not cleaned up after aborting resharding",
);

reshardingTest.teardown();
