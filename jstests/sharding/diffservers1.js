(function() {

    var s = new ShardingTest({name: "diffservers1", shards: 2});

    assert.eq(2, s.config.shards.count(), "server count wrong");
    assert.eq(0, s._connections[0].getDB("config").shards.count(), "shouldn't be here");
    assert.eq(0, s._connections[1].getDB("config").shards.count(), "shouldn't be here");

    test1 = s.getDB("test1").foo;
    test1.save({a: 1});
    test1.save({a: 2});
    test1.save({a: 3});
    assert.eq(3, test1.count());

    assert(!s.admin.runCommand({addshard: "sdd$%"}).ok, "bad hostname");

    var portWithoutHostRunning = allocatePort();
    assert(!s.admin.runCommand({addshard: "127.0.0.1:" + portWithoutHostRunning}).ok,
           "host not up");
    assert(!s.admin.runCommand({addshard: "10.0.0.1:" + portWithoutHostRunning}).ok,
           "allowed shard in IP when config is localhost");

    s.stop();

})();
