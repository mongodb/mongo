import {ShardTransitionUtil} from "jstests/libs/shard_transition_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

var s = new ShardingTest({shards: 2, other: {enableBalancer: true}});
var config = s.s0.getDB('config');

assert.commandWorked(
    s.s0.adminCommand({enableSharding: 'needToMove', primaryShard: s.shard0.shardName}));

var topologyTime0 = config.shards.findOne({_id: s.shard0.shardName}).topologyTime;
var topologyTime1 = config.shards.findOne({_id: s.shard1.shardName}).topologyTime;
assert.gt(topologyTime1, topologyTime0);

// removeShard is not permited on shard0 (the configShard) if configShard is enabled, so we want
// to use transitionToDedicatedConfigServer instead
var removeShardOrTransitionToDedicated =
    TestData.configShard ? "transitionToDedicatedConfigServer" : "removeShard";

// First remove puts in draining mode, the second tells me a db needs to move, the third
// actually removes
assert.commandWorked(s.s0.adminCommand({[removeShardOrTransitionToDedicated]: s.shard0.shardName}));

// Can't make all shards in the cluster draining
assert.commandFailedWithCode(s.s0.adminCommand({removeshard: s.shard1.shardName}),
                             ErrorCodes.IllegalOperation);

var removeResult = assert.commandWorked(
    s.s0.adminCommand({[removeShardOrTransitionToDedicated]: s.shard0.shardName}));
assert.eq(removeResult.dbsToMove, ['needToMove'], "didn't show db to move");
assert(removeResult.note !== undefined);

s.s0.getDB('needToMove').dropDatabase();

// Ensure the balancer moves the config.system.sessions collection chunks out of the shard being
// removed
s.awaitBalancerRound();

if (TestData.configShard) {
    // A config shard can't be removed until all range deletions have finished.
    ShardTransitionUtil.waitForRangeDeletions(s.s);
}

removeResult = assert.commandWorked(
    s.s0.adminCommand({[removeShardOrTransitionToDedicated]: s.shard0.shardName}));
assert.eq('completed', removeResult.state, 'Shard was not removed: ' + tojson(removeResult));

var existingShards = config.shards.find({}).toArray();
assert.eq(
    1, existingShards.length, "Removed server still appears in count: " + tojson(existingShards));

var topologyTime2 = existingShards[0].topologyTime;
assert.gt(topologyTime2, topologyTime1);

assert.commandFailed(s.s0.adminCommand({removeshard: s.shard1.shardName}));

s.stop();
