// Tests whether new sharding is detected on insert by mongos
(function() {
    'use strict';

    var st = new ShardingTest({shards: 10, mongos: 3});

    var mongosA = st.s0;
    var mongosB = st.s1;
    var mongosC = st.s2;

    var admin = mongosA.getDB("admin");
    var config = mongosA.getDB("config");

    var collA = mongosA.getCollection("foo.bar");
    var collB = mongosB.getCollection("" + collA);
    var collC = mongosB.getCollection("" + collA);

    var shards = config.shards.find().sort({_id: 1}).toArray();

    assert.commandWorked(admin.runCommand({enableSharding: "" + collA.getDB()}));
    st.ensurePrimaryShard(collA.getDB().getName(), shards[1]._id);
    assert.commandWorked(admin.runCommand({shardCollection: "" + collA, key: {_id: 1}}));

    jsTestLog("Splitting up the collection...");

    // Split up the collection
    for (var i = 0; i < shards.length; i++) {
        assert.commandWorked(admin.runCommand({split: "" + collA, middle: {_id: i}}));
        assert.commandWorked(
            admin.runCommand({moveChunk: "" + collA, find: {_id: i}, to: shards[i]._id}));
    }

    mongosB.getDB("admin").runCommand({flushRouterConfig: 1});
    mongosC.getDB("admin").runCommand({flushRouterConfig: 1});

    printjson(collB.count());
    printjson(collC.count());

    // Change up all the versions...
    for (var i = 0; i < shards.length; i++) {
        assert.commandWorked(admin.runCommand(
            {moveChunk: "" + collA, find: {_id: i}, to: shards[(i + 1) % shards.length]._id}));
    }

    // Make sure mongos A is up-to-date
    mongosA.getDB("admin").runCommand({flushRouterConfig: 1});

    st.printShardingStatus(true);

    jsTestLog("Running count!");

    printjson(collB.count());
    printjson(collC.find().toArray());

    st.stop();

})();
