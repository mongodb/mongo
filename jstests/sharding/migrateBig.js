(function() {
    'use strict';

    var s = new ShardingTest({name: "migrateBig", shards: 2, other: {chunkSize: 1}});

    assert.writeOK(
        s.config.settings.update({_id: "balancer"}, {$set: {_waitForDelete: true}}, true));
    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    s.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {x: 1}}));

    var db = s.getDB("test");
    var coll = db.foo;

    var big = "";
    while (big.length < 10000)
        big += "eliot";

    var bulk = coll.initializeUnorderedBulkOp();
    for (var x = 0; x < 100; x++) {
        bulk.insert({x: x, big: big});
    }
    assert.writeOK(bulk.execute());

    assert.commandWorked(s.s0.adminCommand({split: "test.foo", middle: {x: 30}}));
    assert.commandWorked(s.s0.adminCommand({split: "test.foo", middle: {x: 66}}));
    assert.commandWorked(s.s0.adminCommand(
        {movechunk: "test.foo", find: {x: 90}, to: s.getOther(s.getPrimaryShard("test")).name}));

    s.printShardingStatus();

    print("YO : " + s.getPrimaryShard("test").host);
    var direct = new Mongo(s.getPrimaryShard("test").host);
    print("direct : " + direct);

    var directDB = direct.getDB("test");

    for (var done = 0; done < 2 * 1024 * 1024; done += big.length) {
        assert.writeOK(directDB.foo.insert({x: 50 + Math.random(), big: big}));
    }

    s.printShardingStatus();

    // This is a large chunk, which should not be able to move
    assert.commandFailed(s.s0.adminCommand(
        {movechunk: "test.foo", find: {x: 50}, to: s.getOther(s.getPrimaryShard("test")).name}));

    for (var i = 0; i < 20; i += 2) {
        try {
            assert.commandWorked(s.s0.adminCommand({split: "test.foo", middle: {x: i}}));
        } catch (e) {
            // We may have auto split on some of these, which is ok
            print(e);
        }
    }

    s.printShardingStatus();

    s.startBalancer();

    assert.soon(function() {
        var x = s.chunkDiff("foo", "test");
        print("chunk diff: " + x);
        return x < 2;
    }, "no balance happened", 8 * 60 * 1000, 2000);

    s.stop();
})();
