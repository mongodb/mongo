// Tests write passthrough
(function() {
'use strict';

var s = new ShardingTest({shards: 2, mongos: 2});
var s2 = s.s1;

assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
s.ensurePrimaryShard('test', s.shard1.shardName);
assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {num: 1}}));

// Ensure that the second mongos will see the movePrimary
s.configRS.awaitLastOpCommitted();

s.getDB("test").foo.save({num: 1});
s.getDB("test").foo.save({num: 2});
s.getDB("test").foo.save({num: 3});
s.getDB("test").foo.save({num: 4});
s.getDB("test").foo.save({num: 5});
s.getDB("test").foo.save({num: 6});
s.getDB("test").foo.save({num: 7});

assert.eq(7, s.getDB("test").foo.find().toArray().length, "normal A");
assert.eq(7, s2.getDB("test").foo.find().toArray().length, "other A");

assert.commandWorked(s.s0.adminCommand({split: "test.foo", middle: {num: 4}}));
assert.commandWorked(s.s0.adminCommand({
    movechunk: "test.foo",
    find: {num: 3},
    to: s.getOther(s.getPrimaryShard("test")).name,
    _waitForDelete: true
}));

assert(s.shard0.getDB("test").foo.find().toArray().length > 0, "blah 1");
assert(s.shard1.getDB("test").foo.find().toArray().length > 0, "blah 2");
assert.eq(7,
          s.shard0.getDB("test").foo.find().toArray().length +
              s.shard1.getDB("test").foo.find().toArray().length,
          "blah 3");

assert.eq(7, s.s0.getDB("test").foo.find().toArray().length, "normal B");
assert.eq(7, s2.getDB("test").foo.find().toArray().length, "other B");

assert.commandWorked(s.s0.adminCommand({split: "test.foo", middle: {num: 2}}));
s.printChunks();

print("* A");

assert.eq(7, s.s0.getDB("test").foo.find().toArray().length, "normal B 1");

s2.getDB("test").foo.save({num: 2});

assert.soon(function() {
    return 8 == s2.getDB("test").foo.find().toArray().length;
}, "other B 2", 5000, 100);

assert.eq(2, s.onNumShards("test", "foo"), "on 2 shards");

s.stop();
})();
