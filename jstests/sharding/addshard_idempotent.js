// Tests that adding an equivalent shard multiple times returns success.
(function() {
    'use strict';

    var st = new ShardingTest({name: "add_shard_idempotent", shards: 0});

    jsTestLog("Testing adding a standalone shard multiple times");
    var shard1 = MongoRunner.runMongod({});
    assert.commandWorked(
        st.admin.runCommand({addshard: shard1.host, name: "newShard1", maxSize: 1024}));

    // Running the identical addShard command should succeed.
    assert.commandWorked(
        st.admin.runCommand({addshard: shard1.host, name: "newShard1", maxSize: 1024}));

    // Trying to add the same shard with different options should fail
    assert.commandFailed(
        st.admin.runCommand({addshard: shard1.host, name: "newShard1"}));  // No maxSize

    assert.commandFailed(st.admin.runCommand(
        {addshard: shard1.host, name: "a different shard name", maxSize: 1024}));

    jsTestLog("Testing adding a replica set shard multiple times");
    var shard2 = new ReplSetTest({name: 'rsShard', nodes: 3});
    shard2.startSet();
    shard2.initiate();
    shard2.getPrimary();  // Wait for there to be a primary
    var shard2SeedList1 = shard2.name + "/" + shard2.nodes[0].host;
    var shard2SeedList2 = shard2.name + "/" + shard2.nodes[2].host;

    assert.commandWorked(st.admin.runCommand({addshard: shard2SeedList1, name: "newShard2"}));

    // Running the identical addShard command should succeed.
    assert.commandWorked(st.admin.runCommand({addshard: shard2SeedList1, name: "newShard2"}));

    // We can only compare replica sets by their set name, so calling addShard with a different
    // seed list should still be considered a successful no-op.
    assert.commandWorked(st.admin.runCommand({addshard: shard2SeedList2, name: "newShard2"}));

    // Verify that the config.shards collection looks right.
    var shards = st.s.getDB('config').shards.find().toArray();
    assert.eq(2, shards.length);
    for (var i = 0; i < shards.length; i++) {
        var shard = shards[i];
        if (shard._id == 'newShard1') {
            assert.eq(shard1.host, shard.host);
            assert.eq(1024, shard.maxSize);
        } else {
            assert.eq('newShard2', shard._id);
            assert.eq(shard2.getURL(), shard.host);
        }
    }

    st.stop();

})();
