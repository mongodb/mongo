/**
 * Tests that the resharding coordinator recovers its abort decision after a primary failover.
 */
(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest(
    {enableElections: true, logComponentVerbosity: tojson({sharding: 2, network: 4})});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

const mongos = sourceCollection.getMongo();
const ns = sourceCollection.getFullName();
let topology = DiscoverTopology.findConnectedNodes(mongos);

const recipientShardNames = reshardingTest.recipientShardNames;
const recipient = new Mongo(topology.shards[recipientShardNames[0]].primary);

// We have the recipient shard fail the _shardsvrAbortReshardCollection to synchronize around
//   (1) the resharding coordinator having persisted its abort decision locally,
//   (2) the resharding coordinator having waited for its abort decision to become majority
//       committed, and
//   (3) the resharding coordinator not yet having finished delivering the abort decision to all of
//       the participant shards.
const shardsvrAbortReshardCollectionFailpoint = configureFailPoint(recipient, "failCommand", {
    failInternalCommands: true,
    errorCode: ErrorCodes.HostUnreachable,
    failCommands: ["_shardsvrAbortReshardCollection"],
});

// We pause the _configsvrReshardCollection command upon joining an existing ReshardingCoordinator
// instance on all of the config server replica set because we don't know which node will be elected
// primary from calling stepUpNewPrimaryOnShard().
const configsvrConnections = topology.configsvr.nodes.map(host => new Mongo(host));
const reshardCollectionJoinedFailPointsList = configsvrConnections.map(
    conn => configureFailPoint(conn, "reshardCollectionJoinedExistingOperation"));

let awaitAbort;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        // Wait until participants are aware of the resharding operation.
        reshardingTest.awaitCloneTimestampChosen();

        awaitAbort = startParallelShell(funWithArgs(function(ns) {
                                            db.adminCommand({abortReshardCollection: ns});
                                        }, ns), mongos.port);

        // Wait for the coordinator to have persisted its decision to abort the resharding operation
        // as a result of the abortReshardCollection command being processed.
        assert.soon(() => {
            const coordinatorDoc =
                mongos.getCollection("config.reshardingOperations").findOne({ns: ns});

            return coordinatorDoc !== null && coordinatorDoc.state === "aborting";
        });
    },
    {
        expectedErrorCode: ErrorCodes.ReshardCollectionAborted,
        postDecisionPersistedFn: () => {
            shardsvrAbortReshardCollectionFailpoint.wait();

            // Mongos automatically retries the abortReshardCollection command on retryable errors.
            // We interrupt the abortReshardCollection command running on mongos to verify that the
            // ReshardingCoordinator recovers the decision on its own.
            const ops = mongos.getDB("admin")
                            .aggregate([
                                {$currentOp: {localOps: true}},
                                {$match: {"command.abortReshardCollection": ns}}
                            ])
                            .toArray();

            assert.neq([], ops, "failed to find abortReshardCollection command running on mongos");
            assert.eq(
                1,
                ops.length,
                () =>
                    `found more than one abortReshardCollection command on mongos: ${tojson(ops)}`);

            assert.commandWorked(mongos.getDB("admin").killOp(ops[0].opid));

            reshardingTest.stepUpNewPrimaryOnShard(reshardingTest.configShardName);

            // After a stepdown, the _configsvrReshardCollection command will be retried by the
            // primary shard. We use the reshardCollectionJoinedExistingOperation failpoint to
            // ensure the primary shard upon retrying finds the ongoing resharding operation on the
            // new config server primary. It would otherwise be possible for the
            // reshardingPauseCoordinatorBeforeCompletion failpoint to be released by the
            // ReshardingTest fixture after this function returns, for the ongoing resharding
            // operation to complete, and for the retried _configsvrReshardCollection command to
            // spawn an entirely new resharding operation which won't get aborted by the test
            // client.
            topology = DiscoverTopology.findConnectedNodes(mongos);
            const configsvrPrimary = new Mongo(topology.configsvr.primary);
            const idx = reshardCollectionJoinedFailPointsList.findIndex(fp => fp.conn.host ===
                                                                            configsvrPrimary.host);
            reshardCollectionJoinedFailPointsList[idx].wait();
            reshardCollectionJoinedFailPointsList.forEach(fp => fp.off());
            shardsvrAbortReshardCollectionFailpoint.off();
        },
    });

awaitAbort();

reshardingTest.teardown();
})();
