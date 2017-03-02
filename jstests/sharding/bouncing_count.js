// Tests whether new sharding is detected on insert by bongos
(function() {
    'use strict';

    var st = new ShardingTest({shards: 10, bongos: 3});

    var bongosA = st.s0;
    var bongosB = st.s1;
    var bongosC = st.s2;

    var admin = bongosA.getDB("admin");
    var config = bongosA.getDB("config");

    var collA = bongosA.getCollection("foo.bar");
    var collB = bongosB.getCollection("" + collA);
    var collC = bongosB.getCollection("" + collA);

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

    bongosB.getDB("admin").runCommand({flushRouterConfig: 1});
    bongosC.getDB("admin").runCommand({flushRouterConfig: 1});

    printjson(collB.count());
    printjson(collC.count());

    // Change up all the versions...
    for (var i = 0; i < shards.length; i++) {
        assert.commandWorked(admin.runCommand(
            {moveChunk: "" + collA, find: {_id: i}, to: shards[(i + 1) % shards.length]._id}));
    }

    // Make sure bongos A is up-to-date
    bongosA.getDB("admin").runCommand({flushRouterConfig: 1});

    jsTestLog("Running count!");

    printjson(collB.count());
    printjson(collC.find().toArray());

    st.stop();

})();
