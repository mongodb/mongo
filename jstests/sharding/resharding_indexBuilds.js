/**
 * Tests that the donor's state will transition to kError and reshardCollection will fail if there
 * is an ongoing index build.
 *
 * @tags: [
 *   uses_atclustertime,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 1});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const inputCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

// Insert a document so that the index build will hit the failpoint.
assert.commandWorked(inputCollection.insert([
    {_id: 0, oldKey: -10, newKey: -10},
]));

const mongos = inputCollection.getMongo();
const recipientShardNames = reshardingTest.recipientShardNames;

const topology = DiscoverTopology.findConnectedNodes(mongos);
const donor_host = topology.shards[donorShardNames[0]].primary;
const donor0 = new Mongo(donor_host);

// Create an inProgress index build.
const createIndexThread = new Thread(function(host) {
    const con = new Mongo(host).getCollection("reshardingDb.coll");
    return con.createIndexes([{newKey: 1}]);
}, donor_host);
let createIndexFailpoint = configureFailPoint(donor0, "hangIndexBuildBeforeCommit");
createIndexThread.start();
createIndexFailpoint.wait();

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {},
    {expectedErrorCode: ErrorCodes.BackgroundOperationInProgressForNamespace});

// Resume index build.
createIndexFailpoint.off();
createIndexThread.join();
assert.commandWorked(createIndexThread.returnData());

reshardingTest.teardown();
