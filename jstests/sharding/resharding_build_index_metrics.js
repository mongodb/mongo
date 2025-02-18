/**
 * Tests that during resharding building index phase, we can see how many indexes to build and how
 * many indexes are built.
 *
 * @tags: [
 *  requires_fcv_72,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 2, enableElections: true});
reshardingTest.setup();

const kDbName = 'reshardingDb';
const kCollName = 'resharding_build_index_metrics';
const ns = kDbName + '.' + kCollName;

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

// Create an index on oldKey.
assert.commandWorked(
    mongos.getCollection(ns).insert([{oldKey: 1, newKey: -1}, {oldKey: 2, newKey: -2}]));
assert.commandWorked(mongos.getCollection(ns).createIndex({oldKey: 1}));
const hangAfterInitializingIndexBuildFailPoint =
    configureFailPoint(recipient, "hangAfterInitializingIndexBuild");

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        hangAfterInitializingIndexBuildFailPoint.wait();

        jsTestLog("Entered building index phase, check currentOp");
        const report = recipient.getDB("admin").currentOp(
            {ns, desc: {$regex: 'ReshardingMetricsRecipientService'}});
        assert.eq(report.inprog.length, 1);
        const curOp = report.inprog[0];
        jsTestLog("Fetched currentOp: " + tojson(curOp));
        // There should be 2 indexes in progress: oldKey and newKey.
        assert.eq(curOp["indexesToBuild"] - curOp["indexesBuilt"], 2);
        hangAfterInitializingIndexBuildFailPoint.off();
    });

reshardingTest.teardown();
