// This tests sharding an existing collection that both shards are aware of (SERVER-2828)
(function() {

    var s1 = new ShardingTest({name: "multi_mongos2a", shards: 2, mongos: 2});
    s2 = s1._mongos[1];

    s1.adminCommand({enablesharding: "test"});
    s1.ensurePrimaryShard('test', 'shard0001');
    s1.adminCommand({shardcollection: "test.foo", key: {num: 1}});

    s1.config.databases.find().forEach(printjson);

    s1.getDB('test').existing.insert({_id: 1});
    assert.eq(1, s1.getDB('test').existing.count({_id: 1}));
    assert.eq(1, s2.getDB('test').existing.count({_id: 1}));

    s2.adminCommand({shardcollection: "test.existing", key: {_id: 1}});
    assert.eq(true, s2.getDB('test').existing.stats().sharded);

    res = s2.getDB("admin").runCommand({
        moveChunk: "test.existing",
        find: {_id: 1},
        to: s1.getOther(s1.getPrimaryShard("test")).name
    });

    assert.eq(1, res.ok, tojson(res));

    s1.adminCommand({flushRouterConfig: 1});

    assert.eq(1, s1.getDB('test').existing.count({_id: 1}));  // SERVER-2828
    assert.eq(1, s2.getDB('test').existing.count({_id: 1}));

    s1.stop();

})();
