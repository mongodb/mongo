//
// Tests that only a correct major-version is needed to connect to a shard via bongos
//
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1, bongos: 2});

    var bongos = st.s0;
    var staleBongos = st.s1;
    var admin = bongos.getDB("admin");
    var config = bongos.getDB("config");
    var coll = bongos.getCollection("foo.bar");

    // Shard collection
    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

    // Make sure our stale bongos is up-to-date with no splits
    staleBongos.getCollection(coll + "").findOne();

    // Run one split
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));

    // Make sure our stale bongos is not up-to-date with the split
    printjson(admin.runCommand({getShardVersion: coll + ""}));
    printjson(staleBongos.getDB("admin").runCommand({getShardVersion: coll + ""}));

    // Compare strings b/c timestamp comparison is a bit weird
    assert.eq(Timestamp(1, 2), admin.runCommand({getShardVersion: coll + ""}).version);
    assert.eq(Timestamp(1, 0),
              staleBongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

    // See if our stale bongos is required to catch up to run a findOne on an existing connection
    staleBongos.getCollection(coll + "").findOne();

    printjson(staleBongos.getDB("admin").runCommand({getShardVersion: coll + ""}));

    assert.eq(Timestamp(1, 0),
              staleBongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

    // See if our stale bongos is required to catch up to run a findOne on a new connection
    staleBongos = new Bongo(staleBongos.host);
    staleBongos.getCollection(coll + "").findOne();

    printjson(staleBongos.getDB("admin").runCommand({getShardVersion: coll + ""}));

    assert.eq(Timestamp(1, 0),
              staleBongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

    st.stop();

})();
