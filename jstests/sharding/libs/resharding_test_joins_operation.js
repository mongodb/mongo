import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getUUIDFromConfigCollections, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";

/**
 * Tests that if a resharding-family command is issued while there is an ongoing operation
 * for the same collection, the command joins with the ongoing instance.
 *
 * This is a generic helper that consolidates the common test pattern across:
 * - reshardCollection
 * - moveCollection
 * - unshardCollection
 * - rewriteCollection
 *
 * @param reshardingTest - a ReshardingTest instance that has been set up with a collection
 * @param opType - the operation type: "reshardCollection", "moveCollection",
 *                 "unshardCollection", or "rewriteCollection"
 * @param operationArgs - arguments for the background operation (e.g., newShardKeyPattern,
 *                        newChunks, toShard)
 * @param makeJoiningThreadFn - a function that returns a Thread object to run the joining
 *                              command. Receives (mongosHost, ns, extraArgs) as parameters.
 * @param joiningThreadExtraArgs - extra arguments to pass to makeJoiningThreadFn (e.g., toShard)
 */
export function runJoinsExistingOperationTest(
    reshardingTest,
    {opType, operationArgs, makeJoiningThreadFn, joiningThreadExtraArgs = {}},
) {
    const mongos = reshardingTest.getMongos();
    const ns = reshardingTest.getSourceNamespace();
    const topology = DiscoverTopology.findConnectedNodes(mongos);
    const donorShardNames = reshardingTest.donorShardNames;

    const getTempUUID = (tempNs) => {
        const tempCollection = mongos.getCollection(tempNs);
        return getUUIDFromConfigCollections(mongos, tempCollection.getFullName());
    };

    // All operations use the same failpoints on the donor shard.
    const donorShardConn = new Mongo(topology.shards[donorShardNames[0]].nodes[0]);

    const pauseFailpoint = configureFailPoint(donorShardConn, "reshardingPauseRecipientDuringCloning");
    const shorterLockTimeout = configureFailPoint(donorShardConn, "overrideDDLLockTimeout", {"timeoutMillisecs": 500});

    const joiningThread = makeJoiningThreadFn(mongos.host, ns, joiningThreadExtraArgs);

    // Fulfilled once the first command creates the temporary collection.
    let expectedUUIDAfterReshardingCompletes = undefined;

    // Select the appropriate with*InBackground method.
    const withOperationInBackgroundFn = {
        "reshardCollection": (args, fn) => reshardingTest.withReshardingInBackground(args, fn),
        "moveCollection": (args, fn) => reshardingTest.withMoveCollectionInBackground(args, fn),
        "unshardCollection": (args, fn) => reshardingTest.withUnshardCollectionInBackground(args, fn),
        "rewriteCollection": (args, fn) => reshardingTest.withRewriteCollectionInBackground(args, fn),
    }[opType];

    assert(withOperationInBackgroundFn, `Unknown operation type: ${opType}`);

    withOperationInBackgroundFn(operationArgs, (tempNs) => {
        pauseFailpoint.wait();

        // The UUID of the temporary resharding collection should become the UUID of the
        // original collection once resharding has completed.
        expectedUUIDAfterReshardingCompletes = getTempUUID(tempNs);

        const joinedFP = configureFailPoint(donorShardConn, "shardsvrReshardCollectionJoinedExistingOperation");

        joiningThread.start();

        // Hitting the joined failpoint is confirmation that the command (same collection
        // as the ongoing operation) gets joined with the ongoing operation.
        joinedFP.wait();

        joinedFP.off();
        pauseFailpoint.off();
    });

    joiningThread.join();
    shorterLockTimeout.off();

    // Confirm the UUID for the namespace matches the temporary collection's UUID before
    // the second command was issued.
    assert.neq(expectedUUIDAfterReshardingCompletes, undefined);
    const sourceCollection = mongos.getCollection(ns);
    const finalSourceCollectionUUID = getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
    assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);
}
