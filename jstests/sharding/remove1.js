(function() {

    var s = new ShardingTest({name: "remove_shard1", shards: 2});

    assert.eq(2, s.config.shards.count(), "initial server count wrong");

    assert.writeOK(
        s.config.databases.insert({_id: 'needToMove', partitioned: false, primary: 'shard0000'}));

    // Returns an error when trying to remove a shard that doesn't exist.
    assert.commandFailed(s.admin.runCommand({removeshard: "shardz"}));

    // first remove puts in draining mode, the second tells me a db needs to move, the third
    // actually removes
    assert(s.admin.runCommand({removeshard: "shard0000"}).ok, "failed to start draining shard");
    assert(!s.admin.runCommand({removeshard: "shard0001"}).ok, "allowed two draining shards");
    assert.eq(s.admin.runCommand({removeshard: "shard0000"}).dbsToMove,
              ['needToMove'],
              "didn't show db to move");
    s.getDB('needToMove').dropDatabase();
    assert(s.admin.runCommand({removeshard: "shard0000"}).ok, "failed to remove shard");
    assert.eq(1, s.config.shards.count(), "removed server still appears in count");

    assert(!s.admin.runCommand({removeshard: "shard0001"}).ok, "allowed removing last shard");

    // should create a shard0002 shard
    var conn = MongoRunner.runMongod({});
    assert(s.admin.runCommand({addshard: conn.host}).ok, "failed to add shard");
    assert.eq(2, s.config.shards.count(), "new server does not appear in count");

    MongoRunner.stopMongod(conn);
    s.stop();

})();
