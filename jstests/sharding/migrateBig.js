(function() {
'use strict';

load("jstests/libs/feature_flag_util.js");

var s = new ShardingTest(
    {name: "migrateBig", shards: 2, other: {chunkSize: 1, enableAutoSplit: false}});

assert.commandWorked(
    s.config.settings.update({_id: "balancer"}, {$set: {_waitForDelete: true}}, true));
assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
s.ensurePrimaryShard('test', s.shard1.shardName);
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
assert.commandWorked(bulk.execute());

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
    assert.commandWorked(directDB.foo.insert({x: 50 + Math.random(), big: big}));
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

s.awaitBalance('foo', 'test', 60 * 1000);

s.stop();
})();
