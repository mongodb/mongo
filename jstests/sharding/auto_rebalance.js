// Validates that auto rebalancing works and reaches quiescent state when replica sets are used as
// shards
(function() {
    'use strict';

    var st = new ShardingTest({mongos: 1, shards: 2, chunkSize: 1, rs: {nodes: 3}});

    assert.writeOK(st.getDB("config").settings.update(
        {_id: "balancer"}, {$set: {"_secondaryThrottle": false}}, {upsert: true}));

    assert.commandWorked(
        st.getDB("admin").runCommand({enableSharding: "TestDB_auto_rebalance_rs"}));
    assert.commandWorked(st.getDB("admin").runCommand(
        {shardCollection: "TestDB_auto_rebalance_rs.foo", key: {x: 1}}));

    var dbTest = st.getDB("TestDB_auto_rebalance_rs");

    var num = 100000;
    var bulk = dbTest.foo.initializeUnorderedBulkOp();
    for (var i = 0; i < num; i++) {
        bulk.insert(
            {_id: i, x: i, abc: "defg", date: new Date(), str: "all the talk on the market"});
    }
    assert.writeOK(bulk.execute());

    // Wait for the rebalancing to kick in
    st.startBalancer();

    assert.soon(function() {
        var s1Chunks = st.getDB("config").chunks.count({shard: "test-rs0"});
        var s2Chunks = st.getDB("config").chunks.count({shard: "test-rs1"});
        var total = st.getDB("config").chunks.count({ns: "TestDB_auto_rebalance_rs.foo"});

        print("chunks: " + s1Chunks + " " + s2Chunks + " " + total);

        return s1Chunks > 0 && s2Chunks > 0 && (s1Chunks + s2Chunks == total);
    }, "Chunks failed to balance", 60000, 5000);

    // Ensure the range deleter quiesces
    st.rs0.awaitReplication(120000);
    st.rs1.awaitReplication(120000);

    st.stop();
})();
