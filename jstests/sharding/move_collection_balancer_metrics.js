/**
 * Tests that _configvrReshardCollection command with provenance set to balancerMoveCollection
 * increments the appropriate metrics in the server status.
 *
 * @tags: [
 *   uses_atclustertime,
 *   requires_fcv_72,
 *   featureFlagReshardingImprovements,
 *   featureFlagMoveCollection,
 *  # TODO (SERVER-87812) Remove multiversion_incompatible tag
 *   multiversion_incompatible,
 *   assumes_balancer_off,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest();
reshardingTest.setup();

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;
const sourceCollection =
    reshardingTest.createUnshardedCollection({ns: ns, primaryShardName: donorShardNames[0]});

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const configsvr = new Mongo(topology.configsvr.nodes[0]);

assert.commandWorked(configsvr.adminCommand({
    _configsvrReshardCollection: ns,
    key: {_id: 1},
    numInitialChunks: 1,
    shardDistribution: [{shard: recipientShardNames[0]}],
    forceRedistribution: true,
    provenance: "balancerMoveCollection",
    writeConcern: {w: "majority"}
}));

const shardingMetrics = configsvr.getDB('admin').serverStatus({}).shardingStatistics
assert.eq(shardingMetrics.moveCollection, undefined);
const balancerMetrics = shardingMetrics.balancerMoveCollection;

assert.eq(balancerMetrics.countStarted, 1);
assert.eq(balancerMetrics.countSucceeded, 1);
assert.eq(balancerMetrics.countFailed, 0);
assert.eq(balancerMetrics.countCanceled, 0);

reshardingTest.teardown();
