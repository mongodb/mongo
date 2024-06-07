/**
 * Tests that if a moveCollection command is issued while there is an ongoing moveCollection
 * operation for the same collection with the same destination shard, the command joins with the
 * ongoing moveCollection instance.
 *
 * @tags: [
 *   uses_atclustertime,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {getUUIDFromConfigCollections, getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

// Generates a new thread to run moveCollection.
const makeMoveCollectionThread = (mongoSConnectionString, ns, toShard) => {
    return new Thread((mongoSConnectionString, ns, toShard) => {
        const mongoS = new Mongo(mongoSConnectionString);
        assert.commandWorked(mongoS.adminCommand({moveCollection: ns, toShard: toShard}));
    }, mongoSConnectionString, ns, toShard);
};

const getTempUUID = (tempNs) => {
    const tempCollection = mongos.getCollection(tempNs);
    return getUUIDFromConfigCollections(mongos, tempCollection.getFullName());
};

const reshardingTest = new ReshardingTest();
reshardingTest.setup();
const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;
const sourceCollection = reshardingTest.createUnshardedCollection(
    {ns: "reshardingDb.coll", primaryShardName: donorShardNames[0]});

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const donorShard = new Mongo(topology.shards[donorShardNames[0]].nodes[0]);

const pauseDuringCloning = configureFailPoint(donorShard, "reshardingPauseRecipientDuringCloning");
const shorterLockTimeout =
    configureFailPoint(donorShard, "overrideDDLLockTimeout", {'timeoutMillisecs': 500});

const moveCollectionThread =
    makeMoveCollectionThread(mongos.host, sourceCollection.getFullName(), recipientShardNames[0]);

// Fulfilled once the first reshardCollection command creates the temporary collection.
let expectedUUIDAfterReshardingCompletes = undefined;

reshardingTest.withMoveCollectionInBackground({toShard: recipientShardNames[0]}, (tempNs) => {
    pauseDuringCloning.wait();

    // The UUID of the temporary resharding collection should become the UUID of the original
    // collection once resharding has completed.
    expectedUUIDAfterReshardingCompletes = getTempUUID(tempNs);

    const moveCollectionJoinedFP =
        configureFailPoint(donorShard, "shardsvrReshardCollectionJoinedExistingOperation");

    moveCollectionThread.start();

    // Hitting the reshardCollectionJoinedFP is additional confirmation that
    // _configsvrReshardCollection command (identical resharding key and collection as the
    // ongoing operation) gets joined with the ongoing resharding operation.
    moveCollectionJoinedFP.wait();

    moveCollectionJoinedFP.off();
    pauseDuringCloning.off();
});

moveCollectionThread.join();
shorterLockTimeout.off();

// Confirm the UUID for the namespace that was resharded is the same as the temporary collection's
// UUID before the second reshardCollection command was issued.
assert.neq(expectedUUIDAfterReshardingCompletes, undefined);
const finalSourceCollectionUUID =
    getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());
assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

reshardingTest.teardown();
