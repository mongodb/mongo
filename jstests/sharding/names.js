// Test that having replica set names the same as the names of other shards works fine
(function() {

    var st = new ShardingTest({name: "HostNames", shards: 0, mongos: 2, other: {rs: true}});

    var rsA = new ReplSetTest({nodes: 2, name: "rsA"});
    var rsB = new ReplSetTest({nodes: 2, name: "rsB"});

    rsA.startSet();
    rsB.startSet();
    rsA.initiate();
    rsB.initiate();
    rsA.getPrimary();
    rsB.getPrimary();

    var mongos = st.s;
    var config = mongos.getDB("config");
    var admin = mongos.getDB("admin");

    assert(admin.runCommand({addShard: rsA.getURL(), name: rsB.name}).ok);
    printjson(config.shards.find().toArray());

    assert(admin.runCommand({addShard: rsB.getURL(), name: rsA.name}).ok);
    printjson(config.shards.find().toArray());

    assert.eq(2, config.shards.count(), "Error adding a shard");
    assert.eq(
        rsB.getURL(), config.shards.findOne({_id: rsA.name})["host"], "Wrong host for shard rsA");
    assert.eq(
        rsA.getURL(), config.shards.findOne({_id: rsB.name})["host"], "Wrong host for shard rsB");

    // Remove shard
    assert(admin.runCommand({removeshard: rsA.name}).ok, "failed to start draining shard");
    assert(admin.runCommand({removeshard: rsA.name}).ok, "failed to remove shard");

    assert.eq(1, config.shards.count(), "Error removing a shard");
    assert.eq(
        rsA.getURL(), config.shards.findOne({_id: rsB.name})["host"], "Wrong host for shard rsB 2");

    // Re-add shard
    assert(admin.runCommand({addShard: rsB.getURL(), name: rsA.name}).ok);
    printjson(config.shards.find().toArray());

    assert.eq(2, config.shards.count(), "Error re-adding a shard");
    assert.eq(
        rsB.getURL(), config.shards.findOne({_id: rsA.name})["host"], "Wrong host for shard rsA 3");
    assert.eq(
        rsA.getURL(), config.shards.findOne({_id: rsB.name})["host"], "Wrong host for shard rsB 3");

    st.stop();

})();
