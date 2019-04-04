// Tests disabling of autosplit.
(function() {
    'use strict';

    var chunkSizeMB = 1;

    // Autosplit is disabled by default, but specify it anyway in case the default changes,
    // especially since it defaults to the enableBalancer setting.
    var st = new ShardingTest(
        {shards: 1, merizos: 1, other: {chunkSize: chunkSizeMB, enableAutoSplit: false}});

    var data = "x";
    while (data.length < chunkSizeMB * 1024 * 1024) {
        data += data;
    }

    var merizos = st.s0;
    var admin = merizos.getDB("admin");
    var config = merizos.getDB("config");
    var coll = merizos.getCollection("foo.bar");

    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

    for (var i = 0; i < 20; i++) {
        coll.insert({data: data});
    }

    // Make sure we haven't split
    assert.eq(1, config.chunks.find({ns: coll + ""}).count());

    st.stop();

})();
