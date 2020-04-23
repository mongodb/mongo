/**
 * @tags: [
 *   multiversion_incompatible,
 *   uses_multi_shard_transaction,
 *   uses_transactions,
 * ]
 */

(function() {
'use strict';

const st = new ShardingTest({shards: 3});
const mongos = st.s;
const testDb = mongos.getDB("test");

assert.commandWorked(mongos.adminCommand({enablesharding: "test"}));
st.ensurePrimaryShard("test", st.shard1.shardName);

// Set up "test.coll0"
assert.commandWorked(mongos.adminCommand({shardcollection: "test.coll0", key: {x: 1}}));
assert.commandWorked(testDb.adminCommand({split: "test.coll0", middle: {x: 5}}));
assert.commandWorked(testDb.adminCommand({split: "test.coll0", middle: {x: 200}}));

// // Move chunks so each shard has one chunk.
assert.commandWorked(mongos.adminCommand(
    {moveChunk: "test.coll0", find: {x: 2}, to: st.shard0.shardName, _waitForDelete: true}));
assert.commandWorked(mongos.adminCommand(
    {moveChunk: "test.coll0", find: {x: 200}, to: st.shard2.shardName, _waitForDelete: true}));

// Set up "test.coll1"
assert.commandWorked(mongos.adminCommand({shardcollection: "test.coll1", key: {x: 1}}));
assert.commandWorked(testDb.adminCommand({split: "test.coll1", middle: {x: 5}}));

// // Move chunk so only shards 0 and 1 have chunks.
assert.commandWorked(mongos.adminCommand(
    {moveChunk: "test.coll1", find: {x: 2}, to: st.shard0.shardName, _waitForDelete: true}));

function assertShardingStats(initialStats, updatedStats, expectedChanges) {
    for (let [cmd, targetedHostsVals] of Object.entries(expectedChanges)) {
        for (let [numTargetedHosts, count] of Object.entries(targetedHostsVals)) {
            if (!initialStats) {
                assert.eq(NumberInt(updatedStats[cmd][numTargetedHosts]), count);
            } else {
                assert.eq(NumberInt(initialStats[cmd][numTargetedHosts]) + count,
                          NumberInt(updatedStats[cmd][numTargetedHosts]));
            }
        }
    }
}

// ----- Check write targeting stats -----

let serverStatusInitial = testDb.serverStatus();
assert.commandWorked(testDb.coll0.insert({x: 9}));
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"insert": {"oneShard": 1}});

serverStatusInitial = testDb.serverStatus();
let bulk = testDb.coll0.initializeUnorderedBulkOp();
for (var x = 0; x < 10; x++) {
    bulk.insert({x: x});
}
assert.commandWorked(bulk.execute());
assertShardingStats({"insert": {"manyShards": 0}},
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"insert": {"manyShards": 1}});

serverStatusInitial = testDb.serverStatus();
bulk = testDb.unshardedCollection.initializeOrderedBulkOp();
bulk.insert({j: -25});
bulk.insert({j: 8});
assert.commandWorked(bulk.execute());
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"insert": {"unsharded": 1}});

bulk = testDb.coll0.initializeUnorderedBulkOp();
bulk.find({x: 200}).update({$set: {a: -21}});
bulk.find({x: -100}).update({$set: {a: -21}});
bulk.find({x: 45}).update({$set: {a: -21}});
assert.commandWorked(bulk.execute());
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"update": {"allShards": 1}});

serverStatusInitial = testDb.serverStatus();
bulk = testDb.coll0.initializeUnorderedBulkOp();
bulk.insert({x: -20});
bulk.find({x: -20}).update({$set: {a: -21}});
bulk.insert({x: 40});
bulk.insert({x: 90});
assert.commandWorked(bulk.execute());
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"insert": {"oneShard": 2}, "update": {"oneShard": 1}});

// If delete targets more than one shard, we broadcast to all shards if the write is not in a
// transaction
serverStatusInitial = testDb.serverStatus();
assert.commandWorked(testDb.coll0.remove({x: {$lt: 9}}));
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"delete": {"allShards": 1}});

// If delete targets more than one shard, we *do not* broadcast to all shards when the write is in a
// transaction
let session = mongos.startSession();
let sessionDB = session.getDatabase("test");
session.startTransaction();
serverStatusInitial = testDb.serverStatus();
assert.commandWorked(sessionDB.coll0.remove({x: {$lt: 9}}));
session.commitTransaction();
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"delete": {"manyShards": 1}});

serverStatusInitial = testDb.serverStatus();
bulk = testDb.coll1.initializeUnorderedBulkOp();
bulk.insert({x: -100, a: 100});
bulk.insert({x: 100, a: 100});
assert.commandWorked(bulk.execute());
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"insert": {"allShards": 1}});

serverStatusInitial = testDb.serverStatus();
assert.commandWorked(testDb.coll1.update({a: 100}, {$set: {a: -21}}, {multi: true}));
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"update": {"allShards": 1}});

// ----- Check find targeting stats -----

serverStatusInitial = testDb.serverStatus();
let findRes = testDb.coll0.find({"x": {$gt: 8}}).itcount();
assert.gte(findRes, 1);
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"find": {"manyShards": 1}});

serverStatusInitial = testDb.serverStatus();
findRes = testDb.coll0.find({"x": {$lt: 300}}).itcount();
assert.gte(findRes, 1);
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"find": {"allShards": 1}});

serverStatusInitial = testDb.serverStatus();
findRes = testDb.coll0.find({"x": 40}).itcount();
assert.gte(findRes, 1);
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"find": {"oneShard": 1}});

serverStatusInitial = testDb.serverStatus();
findRes = testDb.unshardedCollection.find({"j": 8}).itcount();
assert.gte(findRes, 1);
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"find": {"unsharded": 1}});

serverStatusInitial = testDb.serverStatus();
findRes = testDb.coll1.find({a: -21}).itcount();
assert.gte(findRes, 1);
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"find": {"allShards": 1}});

// ----- Check aggregate targeting stats -----

serverStatusInitial = testDb.serverStatus();
let aggRes =
    testDb.coll0.aggregate([{$match: {x: {$gte: 15, $lte: 100}}}].concat([{$sort: {x: 1}}]))
        .itcount();
assert.gte(aggRes, 1);
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"aggregate": {"oneShard": 1}});

serverStatusInitial = testDb.serverStatus();
aggRes = testDb.coll0.aggregate([{$match: {x: {$gte: -150, $lte: 50}}}].concat([{$sort: {x: 1}}]))
             .itcount();
assert.gte(aggRes, 1);
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"aggregate": {"manyShards": 1}});

serverStatusInitial = testDb.serverStatus();
aggRes = testDb.coll0.aggregate([{$match: {x: {$gte: -150, $lte: 500}}}].concat([{$sort: {x: 1}}]))
             .itcount();
assert.gte(aggRes, 1);
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"aggregate": {"allShards": 1}});

serverStatusInitial = testDb.serverStatus();
aggRes = testDb.unshardedCollection
             .aggregate([{$match: {j: {$gte: -150, $lte: 50}}}].concat([{$sort: {j: 1}}]))
             .itcount();
assert.gte(aggRes, 1);
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"aggregate": {"unsharded": 1}});

serverStatusInitial = testDb.serverStatus();
aggRes =
    testDb.coll1
        .aggregate([{
            $lookup: {from: "unshardedCollection", localField: "x", foreignField: "j", as: "lookup"}
        }])
        .itcount();
assert.gte(aggRes, 1);
assertShardingStats(serverStatusInitial.shardingStatistics.numHostsTargeted,
                    testDb.serverStatus().shardingStatistics.numHostsTargeted,
                    {"aggregate": {"allShards": 1}});

st.stop();
})();
