//
// Test to verify that latency metrics are collected in both currentOp and cumulativeOp
// when batches of oplogs are applied during resharding.
//
// @tags: [
//   requires_fcv_51,
//   uses_atclustertime,
// ]
//

(function() {
'use strict';

load('jstests/libs/discover_topology.js');
load('jstests/sharding/libs/resharding_test_fixture.js');

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const kHistogramTag = "oplogBatchApplyLatencyMillis";
const kDbName = "reshardingDb";
const collName = "coll";
const ns = kDbName + "." + collName;

const donorShardNames = reshardingTest.donorShardNames;
const testColl = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: {x: 1, s: 1},
    chunks: [
        {min: {x: MinKey, s: MinKey}, max: {x: 5, s: 5}, shard: donorShardNames[0]},
        {min: {x: 5, s: 5}, max: {x: MaxKey, s: MaxKey}, shard: donorShardNames[1]},
    ],
});

function getCumulativeOpReport(mongo) {
    const stats = mongo.getDB('admin').serverStatus({});
    assert(stats.hasOwnProperty('shardingStatistics'), stats);
    const shardingStats = stats.shardingStatistics;
    assert(shardingStats.hasOwnProperty('resharding'),
           `Missing resharding section in ${tojson(shardingStats)}`);

    return shardingStats.resharding;
}

function getCurrentOpReport(mongo, role) {
    return mongo.getDB("admin").currentOp(
        {ns: ns, desc: {$regex: 'Resharding' + role + 'Service.*'}});
}

const mongos = testColl.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const recipientShardNames = reshardingTest.recipientShardNames;
const docsToInsert = [
    {_id: 1, x: 0, s: 6, y: 0},  // Stays on shard0.
    {_id: 2, x: 0, s: 0, y: 6},  // Moves to shard1.
    {_id: 3, x: 6, s: 6, y: 0},  // Moves to shard0.
    {_id: 4, x: 6, s: 0, y: 6},  // Stays on shard1.
];

// First test that histogram metrics appear in currentOp.
let batchAppliesInFirstReshard = 0;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {y: 1, s: 1},
        newChunks: [
            {min: {y: MinKey, s: MinKey}, max: {y: 5, s: 5}, shard: recipientShardNames[0]},
            {min: {y: 5, s: 5}, max: {y: MaxKey, s: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {
        reshardingTest.awaitCloneTimestampChosen();
        assert.commandWorked(testColl.insertMany(docsToInsert));
    },
    {
        postCheckConsistencyFn: () => {
            reshardingTest.recipientShardNames.forEach(function(shardName) {
                const report =
                    getCurrentOpReport(new Mongo(topology.shards[shardName].primary), "Recipient");
                assert(report.inprog.length === 1,
                       `expected report.inprog.length === 1, 
                       instead found ${report.inprog.length}`);
                const op = report.inprog[0];
                assert(op.hasOwnProperty(kHistogramTag),
                       `Missing ${kHistogramTag} in ${tojson(op)}`);
                let batchAppliesThisRecipient = op[kHistogramTag]["ops"];
                batchAppliesInFirstReshard += batchAppliesThisRecipient;
            });

            assert(batchAppliesInFirstReshard > 0,
                   `Expected greater than 0 recorded batch applies,
                                                       got ${batchAppliesInFirstReshard} instead.`);
        }
    });

// Next test that histogram metrics accumulate in cumulativeOp.
const collName_2 = "coll2";
const ns_2 = kDbName + "." + collName_2;

const testColl_2 = reshardingTest.createShardedCollection({
    ns: ns_2,
    shardKeyPattern: {x: 1, s: 1},
    chunks: [
        {min: {x: MinKey, s: MinKey}, max: {x: 5, s: 5}, shard: donorShardNames[0]},
        {min: {x: 5, s: 5}, max: {x: MaxKey, s: MaxKey}, shard: donorShardNames[1]},
    ],
});

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {y: 1, s: 1},
        newChunks: [
            {min: {y: MinKey, s: MinKey}, max: {y: 5, s: 5}, shard: recipientShardNames[0]},
            {min: {y: 5, s: 5}, max: {y: MaxKey, s: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {
        reshardingTest.awaitCloneTimestampChosen();
        assert.commandWorked(testColl_2.insertMany(docsToInsert));
    });

let cumulativeBatchApplies = 0;
reshardingTest.recipientShardNames.forEach(function(shardName) {
    let report = getCumulativeOpReport(new Mongo(topology.shards[shardName].primary));
    assert(report.hasOwnProperty(kHistogramTag));
    let cumulativeBatchAppliesThisRecipient = report[kHistogramTag]["ops"];
    cumulativeBatchApplies += cumulativeBatchAppliesThisRecipient;
});

assert(cumulativeBatchApplies > batchAppliesInFirstReshard, `Expected batch oplog applies to accumluate. 
        Instead found ${cumulativeBatchApplies} cumulative applies, compared to ${batchAppliesInFirstReshard} 
        from first reshard operation.`);

reshardingTest.teardown();
})();
