/**
 * Verifies that resharding honors the critical section timeout.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

function setupTest(reshardingTest, namespace, timeout) {
    reshardingTest.setup();

    jsTest.log(`Running test for criticalSectionTimeoutMillis = ${timeout}`);

    const donorShardNames = reshardingTest.donorShardNames;
    const inputCollection = reshardingTest.createShardedCollection({
        ns: namespace,
        shardKeyPattern: {oldKey: 1},
        chunks: [
            {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
            {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
        ],
    });

    const mongos = inputCollection.getMongo();
    const topology = DiscoverTopology.findConnectedNodes(mongos);
    const coordinator = new Mongo(topology.configsvr.nodes[0]);
    assert.commandWorked(coordinator.getDB("admin").adminCommand(
        {setParameter: 1, reshardingCriticalSectionTimeoutMillis: timeout}));

    assert.commandWorked(inputCollection.insert([
        {_id: "stays on shard0", oldKey: -10, newKey: -10},
        {_id: "moves to shard0", oldKey: 10, newKey: -10},
        {_id: "moves to shard1", oldKey: -10, newKey: 10},
        {_id: "stays on shard1", oldKey: 10, newKey: 10},
    ]));
}

// This test will not timeout.
const successReshardingTest =
    new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
const noTimeoutMillis = 8000;
var namespace = `reshardingDb.coll${noTimeoutMillis}`;

setupTest(successReshardingTest, namespace, noTimeoutMillis);

var recipientShardNames = successReshardingTest.recipientShardNames;
successReshardingTest.withReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    newChunks: [
        {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
        {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
    ],
});

successReshardingTest.teardown();

// This test will timeout.
const failureReshardingTest =
    new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
const shouldTimeoutMillis = 0;
namespace = `reshardingDb.coll${shouldTimeoutMillis}`;

setupTest(failureReshardingTest, namespace, shouldTimeoutMillis);

recipientShardNames = failureReshardingTest.recipientShardNames;
failureReshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    (tempNs) => {},
    {expectedErrorCode: ErrorCodes.ReshardingCriticalSectionTimeout});

failureReshardingTest.teardown();
})();
