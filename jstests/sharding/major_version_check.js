//
// Tests that only a correct major-version is needed to connect to a shard via merizos
//
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1, merizos: 2});

    var merizos = st.s0;
    var staleMerizos = st.s1;
    var admin = merizos.getDB("admin");
    var config = merizos.getDB("config");
    var coll = merizos.getCollection("foo.bar");

    // Shard collection
    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

    // Make sure our stale merizos is up-to-date with no splits
    staleMerizos.getCollection(coll + "").findOne();

    // Run one split
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));

    // Make sure our stale merizos is not up-to-date with the split
    printjson(admin.runCommand({getShardVersion: coll + ""}));
    printjson(staleMerizos.getDB("admin").runCommand({getShardVersion: coll + ""}));

    // Compare strings b/c timestamp comparison is a bit weird
    assert.eq(Timestamp(1, 2), admin.runCommand({getShardVersion: coll + ""}).version);
    assert.eq(Timestamp(1, 0),
              staleMerizos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

    // See if our stale merizos is required to catch up to run a findOne on an existing connection
    staleMerizos.getCollection(coll + "").findOne();

    printjson(staleMerizos.getDB("admin").runCommand({getShardVersion: coll + ""}));

    assert.eq(Timestamp(1, 0),
              staleMerizos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

    // See if our stale merizos is required to catch up to run a findOne on a new connection
    staleMerizos = new Merizo(staleMerizos.host);
    staleMerizos.getCollection(coll + "").findOne();

    printjson(staleMerizos.getDB("admin").runCommand({getShardVersion: coll + ""}));

    assert.eq(Timestamp(1, 0),
              staleMerizos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

    st.stop();

})();
