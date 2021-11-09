// Test to verify that latency metrics are collected in both currentOp and cumulativeOp
// during resharding.
//
// @tags: [
//   uses_atclustertime,
// ]
//

(function() {
'use strict';

load('jstests/libs/discover_topology.js');
load('jstests/sharding/libs/resharding_test_fixture.js');

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const kOplogApplierApplyBatchLatencyMillis = "oplogApplierApplyBatchLatencyMillis";
const kCollClonerFillBatchForInsertLatencyMillis = "collClonerFillBatchForInsertLatencyMillis";
const kDocumentsCopied = "documentsCopied";
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

function setParameter(conn, field, value) {
    var cmd = {setParameter: 1};
    cmd[field] = value;
    return conn.adminCommand(cmd);
}

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

function getReshardingMetricsReport(mongo, role) {
    if (role === "Cumulative") {
        return getCumulativeOpReport(mongo);
    } else {
        const report = getCurrentOpReport(mongo, role);
        assert(report.inprog.length === 1,
               `expected report.inprog.length === 1,
            instead found ${report.inprog.length}`);
        return report.inprog[0];
    }
}

const mongos = testColl.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const recipientShardNames = reshardingTest.recipientShardNames;

recipientShardNames.forEach(function(shardName) {
    const mongo = new Mongo(topology.shards[shardName].primary);

    // Batches from resharding::data_copy::fillBatchForInsert are filled with documents
    // until a document pushes the batch over the expected size. Setting the server
    // parameter 'reshardingCollectionClonerBatchSizeInBytes' to the minimum value of 1
    // ensures that the first document added to the batch will exceed this value, forcing
    // every batch to contain only 1 document.
    assert.commandWorked(setParameter(mongo, "reshardingCollectionClonerBatchSizeInBytes", 1));
});

const docsToInsertBeforeResharding = [
    {_id: 1, x: 0, s: 6, y: 0},  // Stays on shard0.
    {_id: 2, x: 0, s: 0, y: 6},  // Moves to shard1.
    {_id: 3, x: 6, s: 6, y: 0},  // Moves to shard0.
    {_id: 4, x: 6, s: 0, y: 6},  // Stays on shard1.
];
const docsToInsertDuringResharding = [
    {_id: 5, x: 0, s: 6, y: 0},  // Stays on shard0.
    {_id: 6, x: 0, s: 0, y: 6},  // Moves to shard1.
    {_id: 7, x: 6, s: 6, y: 0},  // Moves to shard0.
    {_id: 8, x: 6, s: 0, y: 6},  // Stays on shard1.
];
assert.commandWorked(testColl.insertMany(docsToInsertBeforeResharding));

// First test that histogram metrics appear in currentOp.
let firstReshardBatchApplies = 0;
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
        assert.commandWorked(testColl.insertMany(docsToInsertDuringResharding));
    },
    {
        postCheckConsistencyFn: () => {
            recipientShardNames.forEach(function(shardName) {
                const mongo = new Mongo(topology.shards[shardName].primary);

                const reshardingMetrics = getReshardingMetricsReport(mongo, "Recipient");
                const oplogApplierApplyBatchHist =
                    reshardingMetrics[kOplogApplierApplyBatchLatencyMillis];
                const collClonerFillBatchForInsertHist =
                    reshardingMetrics[kCollClonerFillBatchForInsertLatencyMillis];

                // We expect 1 batch insert per document on each shard, plus 1 empty batch
                // to discover no documents are left.
                const expectedBatchInserts = reshardingMetrics[kDocumentsCopied] + 1;
                const receivedBatchInserts = collClonerFillBatchForInsertHist["ops"];
                assert(expectedBatchInserts == receivedBatchInserts,
                       `expected ${expectedBatchInserts} batch inserts,
                       received ${receivedBatchInserts}`);

                firstReshardBatchApplies += oplogApplierApplyBatchHist["ops"];
            });

            assert(firstReshardBatchApplies > 0,
                   `Expected greater than 0 recorded batch applies,
                    got ${firstReshardBatchApplies} instead.`);
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

assert.commandWorked(testColl_2.insertMany(docsToInsertBeforeResharding));
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
        assert.commandWorked(testColl_2.insertMany(docsToInsertDuringResharding));
    });

let cumulativeBatchApplies = 0;
let cumulativeBatchInserts = 0;
let totalDocumentsCopied = 0;
recipientShardNames.forEach(function(shardName) {
    const mongo = new Mongo(topology.shards[shardName].primary);

    const reshardingMetrics = getReshardingMetricsReport(mongo, "Cumulative");
    const oplogApplierApplyBatchHist = reshardingMetrics[kOplogApplierApplyBatchLatencyMillis];
    const collClonerFillBatchForInsertHist =
        reshardingMetrics[kCollClonerFillBatchForInsertLatencyMillis];

    cumulativeBatchApplies += oplogApplierApplyBatchHist["ops"];
    cumulativeBatchInserts += collClonerFillBatchForInsertHist["ops"];
    totalDocumentsCopied += reshardingMetrics[kDocumentsCopied];
});

// We expect the cumulative number of batch inserts to be equal to the total number of documents
// copied during cloning plus one empty batch for each recipient for both resharding operations.
const expectedCumulativeBatchInserts = totalDocumentsCopied + 2 * recipientShardNames.length;

assert(cumulativeBatchApplies > firstReshardBatchApplies, `Expected batch oplog applies
       to accumluate. Instead found ${cumulativeBatchApplies} cumulative applies,
       compared to ${firstReshardBatchApplies} from first reshard operation.`);
assert(cumulativeBatchInserts == expectedCumulativeBatchInserts, `Expected
       ${expectedCumulativeBatchInserts} cumulative batch inserts. Instead found
       ${cumulativeBatchInserts}`);

reshardingTest.teardown();
})();
