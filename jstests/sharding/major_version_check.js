//
// Tests that only a correct major-version is needed to connect to a shard via mongos
//
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1, mongos: 2});

    var mongos = st.s0;
    var staleMongos = st.s1;
    var admin = mongos.getDB("admin");
    var config = mongos.getDB("config");
    var coll = mongos.getCollection("foo.bar");

    // Shard collection
    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

    // Make sure our stale mongos is up-to-date with no splits
    staleMongos.getCollection(coll + "").findOne();

    // Run one split
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));

    // Make sure our stale mongos is not up-to-date with the split
    printjson(admin.runCommand({getShardVersion: coll + ""}));
    printjson(staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}));

    // Compare strings b/c timestamp comparison is a bit weird
    assert.eq(Timestamp(1, 2), admin.runCommand({getShardVersion: coll + ""}).version);
    assert.eq(Timestamp(1, 0),
              staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

    // See if our stale mongos is required to catch up to run a findOne on an existing connection
    staleMongos.getCollection(coll + "").findOne();

    printjson(staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}));

    assert.eq(Timestamp(1, 0),
              staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

    // See if our stale mongos is required to catch up to run a findOne on a new connection
    staleMongos = new Mongo(staleMongos.host);
    staleMongos.getCollection(coll + "").findOne();

    printjson(staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}));

    assert.eq(Timestamp(1, 0),
              staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

    st.stop();

})();
