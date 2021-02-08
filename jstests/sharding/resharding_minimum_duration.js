/**
 * Verifies that operations honor the minimum resharding duration.
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

function durationOfLastReshardingOperation(coordinator) {
    var resumeMarker;
    coordinator.getDB("admin").adminCommand({getLog: 'global'}).log.forEach(entry => {
        const entryJSON = JSON.parse(entry);
        if (entryJSON.id === 5391801) {
            resumeMarker = entryJSON;
        }
    });

    assert.neq(
        resumeMarker, undefined, 'Unable to find the log entry for resuming the coordinator');

    jsTest.log(`${tojson(resumeMarker)}`);

    const startDate = Date.parse(resumeMarker.attr.startedOn.$date);
    const resumeDate = Date.parse(resumeMarker.attr.resumedOn.$date);
    return resumeDate - startDate;
}

function runTest(reshardingTest, namespace, minimumReshardingDuration) {
    jsTest.log(`Running test for minimumReshardingDuration = ${minimumReshardingDuration}`);

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
    coordinator.getDB("admin").adminCommand(
        {setParameter: 1, reshardingMinimumOperationDurationMillis: minimumReshardingDuration});

    assert.commandWorked(inputCollection.insert([
        {_id: "stays on shard0", oldKey: -10, newKey: -10},
        {_id: "moves to shard0", oldKey: 10, newKey: -10},
        {_id: "moves to shard1", oldKey: -10, newKey: 10},
        {_id: "stays on shard1", oldKey: 10, newKey: 10},
    ]));

    const recipientShardNames = reshardingTest.recipientShardNames;
    reshardingTest.withReshardingInBackground({
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    });

    assert.gte(durationOfLastReshardingOperation(coordinator),
               minimumReshardingDuration,
               'Expected the resharding coordinator to honor the minimum resharding duration');
}

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const durations = [5000, 10000, 20000];  // 5, 10, and 20 seconds
durations.forEach(minimumReshardingDuration =>
                      runTest(reshardingTest,
                              `reshardingDb.coll${minimumReshardingDuration}`,
                              minimumReshardingDuration));

reshardingTest.teardown();
})();
