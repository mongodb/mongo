// This test creates a sharded collection with wiredtiger options.
// When the chunks of this collection get migrated to the other shard,
// the other shard should create the collection with the same options.

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2});

var db = st.s.getDB("test");
let coll = db.sharding_system_namespaces;

// This test relies on the wiredTiger storage engine being compiled
// into the server. Must check shard member for WT as it is not built into mongos.

let storageEngines = st.shard0.getDB("local").getServerBuildInfo().rawData().storageEngines;

print("Supported storage engines: " + storageEngines);

if (Array.contains(storageEngines, "wiredTiger")) {
    function checkCollectionOptions(database) {
        let collectionsInfos = database.getCollectionInfos();
        printjson(collectionsInfos);
        let info = collectionsInfos.filter(function (c) {
            return c.name == "sharding_system_namespaces";
        })[0];
        assert.eq(info.options.storageEngine.wiredTiger.configString, "block_compressor=zlib");
    }

    assert.commandWorked(db.adminCommand({enableSharding: "test", primaryShard: st.shard1.shardName}));
    db.createCollection("sharding_system_namespaces", {
        storageEngine: {wiredTiger: {configString: "block_compressor=zlib"}},
    });

    checkCollectionOptions(db);

    assert.commandWorked(db.adminCommand({shardCollection: coll + "", key: {x: 1}}));

    coll.insert({x: 0});
    coll.insert({x: 10});

    assert.commandWorked(db.adminCommand({split: coll + "", middle: {x: 5}}));

    st.printShardingStatus();

    let primaryShard = st.getPrimaryShard("test");
    let anotherShard = st.getOther(primaryShard);
    assert.commandWorked(db.adminCommand({movechunk: coll + "", find: {x: 5}, to: anotherShard.name}));

    st.printShardingStatus();

    checkCollectionOptions(anotherShard.getDB("test"));
} else {
    print("Skipping test. wiredTiger engine not supported by mongod binary.");
}
st.stop();
