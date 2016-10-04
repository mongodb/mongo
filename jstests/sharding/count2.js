(function() {

    var s1 = new ShardingTest({name: "count2", shards: 2, mongos: 2});
    var s2 = s1._mongos[1];

    s1.adminCommand({enablesharding: "test"});
    s1.ensurePrimaryShard('test', 'shard0001');
    s1.adminCommand({shardcollection: "test.foo", key: {name: 1}});

    var db1 = s1.getDB("test").foo;
    var db2 = s2.getDB("test").foo;

    assert.eq(1, s1.config.chunks.count(), "sanity check A");

    db1.save({name: "aaa"});
    db1.save({name: "bbb"});
    db1.save({name: "ccc"});
    db1.save({name: "ddd"});
    db1.save({name: "eee"});
    db1.save({name: "fff"});

    s1.adminCommand({split: "test.foo", middle: {name: "ddd"}});

    assert.eq(3, db1.count({name: {$gte: "aaa", $lt: "ddd"}}), "initial count mongos1");
    assert.eq(3, db2.count({name: {$gte: "aaa", $lt: "ddd"}}), "initial count mongos2");

    s1.printChunks("test.foo");

    s1.adminCommand({
        movechunk: "test.foo",
        find: {name: "aaa"},
        to: s1.getOther(s1.getPrimaryShard("test")).name,
        _waitForDelete: true
    });

    assert.eq(3, db1.count({name: {$gte: "aaa", $lt: "ddd"}}), "post count mongos1");

    // The second mongos still thinks its shard mapping is valid and accepts a cound
    print("before sleep: " + Date());
    sleep(2000);
    print("after  sleep: " + Date());
    s1.printChunks("test.foo");
    assert.eq(3, db2.find({name: {$gte: "aaa", $lt: "ddd"}}).count(), "post count mongos2");

    db2.findOne();

    assert.eq(3, db2.count({name: {$gte: "aaa", $lt: "ddd"}}));

    assert.eq(4, db2.find().limit(4).count(true));
    assert.eq(4, db2.find().limit(-4).count(true));
    assert.eq(6, db2.find().limit(0).count(true));
    assert.eq(6, db2.getDB().runCommand({count: db2.getName(), limit: 0}).n);

    s1.stop();

})();
