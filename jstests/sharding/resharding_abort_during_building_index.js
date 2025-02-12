/**
 * Tests that when resharding is in building-index phase, abort the resharding operation will exit
 * correctly.
 *
 * @tags: [
 *  requires_fcv_72,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const ns = "reshardingDb.coll";
const reshardingTest = new ReshardingTest({numDonors: 2, enableElections: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});
const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);

const recipientShardNames = reshardingTest.recipientShardNames;
const recipient = new Mongo(topology.shards[recipientShardNames[0]].primary);

assert.commandWorked(
    mongos.getCollection(ns).insert([{oldKey: 1, newKey: -1}, {oldKey: 2, newKey: -2}]));
assert.commandWorked(mongos.getCollection(ns).createIndex({oldKey: 1}));
const hangAfterInitializingIndexBuildFailPoint =
    configureFailPoint(recipient, "hangAfterInitializingIndexBuild");

let awaitAbort;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        reshardingTest.awaitCloneTimestampChosen();
        hangAfterInitializingIndexBuildFailPoint.wait();
        jsTestLog("Hang primary during building index, then abort resharding");

        assert.neq(null, mongos.getCollection("config.reshardingOperations").findOne({ns: ns}));
        awaitAbort =
            startParallelShell(funWithArgs(function(sourceNamespace) {
                                   db.adminCommand({abortReshardCollection: sourceNamespace});
                               }, ns), mongos.port);

        assert.soon(() => {
            const coordinatorDoc =
                mongos.getCollection("config.reshardingOperations").findOne({ns: ns});
            return coordinatorDoc === null || coordinatorDoc.state === "aborting";
        });
    },
    {expectedErrorCode: ErrorCodes.ReshardCollectionAborted});

awaitAbort();
hangAfterInitializingIndexBuildFailPoint.off();

reshardingTest.teardown();
