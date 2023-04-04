(function() {
'use strict';

var s = new ShardingTest({shards: 2, other: {enableBalancer: true}});
var config = s.s0.getDB('config');

assert.commandWorked(s.s0.adminCommand({enableSharding: 'needToMove'}));
s.ensurePrimaryShard('needToMove', s.shard0.shardName);

// Returns an error when trying to remove a shard that doesn't exist.
assert.commandFailedWithCode(s.s0.adminCommand({removeshard: "shardz"}), ErrorCodes.ShardNotFound);

var topologyTime0 = config.shards.findOne({_id: s.shard0.shardName}).topologyTime;
var topologyTime1 = config.shards.findOne({_id: s.shard1.shardName}).topologyTime;
assert.gt(topologyTime1, topologyTime0);

// removeShard is not permited on shard0 (the catalogShard) if catalogShard is enabled, so we want
// to use transitionToDedicatedConfigServer instead
var removeShardOrTransitionToDedicated =
    TestData.catalogShard ? "transitionToDedicatedConfigServer" : "removeShard";

// First remove puts in draining mode, the second tells me a db needs to move, the third
// actually removes
assert.commandWorked(s.s0.adminCommand({[removeShardOrTransitionToDedicated]: s.shard0.shardName}));

// Can't make all shards in the cluster draining
assert.commandFailedWithCode(s.s0.adminCommand({removeshard: s.shard1.shardName}),
                             ErrorCodes.IllegalOperation);

var removeResult = assert.commandWorked(
    s.s0.adminCommand({[removeShardOrTransitionToDedicated]: s.shard0.shardName}));
assert.eq(removeResult.dbsToMove, ['needToMove'], "didn't show db to move");

s.s0.getDB('needToMove').dropDatabase();

// Ensure the balancer moves the config.system.sessions collection chunks out of the shard being
// removed
s.awaitBalancerRound();

removeResult = assert.commandWorked(
    s.s0.adminCommand({[removeShardOrTransitionToDedicated]: s.shard0.shardName}));
assert.eq('completed', removeResult.state, 'Shard was not removed: ' + tojson(removeResult));

var existingShards = config.shards.find({}).toArray();
assert.eq(
    1, existingShards.length, "Removed server still appears in count: " + tojson(existingShards));

var topologyTime2 = existingShards[0].topologyTime;
assert.gt(topologyTime2, topologyTime1);

assert.commandFailed(s.s0.adminCommand({removeshard: s.shard1.shardName}));

// Should create a shard0002 shard
var rs = new ReplSetTest({nodes: 1});
rs.startSet({shardsvr: ""});
rs.initiate();
assert.commandWorked(s.s0.adminCommand({addshard: rs.getURL()}));
assert.eq(2, s.config.shards.count(), "new server does not appear in count");

rs.stopSet();
s.stop();
})();
