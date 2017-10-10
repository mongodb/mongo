(function() {
    'use strict';

    var s = new ShardingTest({shards: 2, other: {enableBalancer: true}});
    var config = s.s0.getDB('config');

    assert.commandWorked(s.s0.adminCommand({enableSharding: 'needToMove'}));
    s.ensurePrimaryShard('needToMove', 'shard0000');

    // Returns an error when trying to remove a shard that doesn't exist.
    assert.commandFailedWithCode(s.s0.adminCommand({removeshard: "shardz"}),
                                 ErrorCodes.ShardNotFound);

    // First remove puts in draining mode, the second tells me a db needs to move, the third
    // actually removes
    assert.commandWorked(s.s0.adminCommand({removeshard: "shard0000"}));

    // Can't have more than one draining shard at a time
    assert.commandFailedWithCode(s.s0.adminCommand({removeshard: "shard0001"}),
                                 ErrorCodes.ConflictingOperationInProgress);
    assert.eq(s.s0.adminCommand({removeshard: "shard0000"}).dbsToMove,
              ['needToMove'],
              "didn't show db to move");

    s.s0.getDB('needToMove').dropDatabase();

    // Ensure the balancer moves the config.system.sessions collection chunks out of the shard being
    // removed
    s.awaitBalancerRound();

    var removeResult = assert.commandWorked(s.s0.adminCommand({removeshard: "shard0000"}));
    assert.eq('completed', removeResult.state, 'Shard was not removed: ' + tojson(removeResult));

    var existingShards = config.shards.find({}).toArray();
    assert.eq(1,
              existingShards.length,
              "Removed server still appears in count: " + tojson(existingShards));

    assert.commandFailed(s.s0.adminCommand({removeshard: "shard0001"}));

    // Should create a shard0002 shard
    var conn = MongoRunner.runMongod({});
    assert.commandWorked(s.s0.adminCommand({addshard: conn.host}));
    assert.eq(2, s.config.shards.count(), "new server does not appear in count");

    MongoRunner.stopMongod(conn);
    s.stop();
})();
