(function() {
'use strict';

let s = new ShardingTest({shards: 2, mongos: 2});
let s2 = s.s1;

assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
s.ensurePrimaryShard('test', s.shard1.shardName);
assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {num: 1}}));

// Ensure that the second mongos will see the movePrimary
s.configRS.awaitLastOpCommitted();

s.s0.getDB("test").foo.save({num: 1});
s.s0.getDB("test").foo.save({num: 2});
s.s0.getDB("test").foo.save({num: 3});
s.s0.getDB("test").foo.save({num: 4});
s.s0.getDB("test").foo.save({num: 5});
s.s0.getDB("test").foo.save({num: 6});
s.s0.getDB("test").foo.save({num: 7});

assert.eq(7, s.s0.getDB("test").foo.find().toArray().length, "normal A");
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
assert.eq(7, s2.getDB("test").foo.find().toArray().length, "other B 2");

print("* B");
assert.eq(7, s.s0.getDB("test").foo.find().toArray().length, "normal B 3");
assert.eq(7, s2.getDB("test").foo.find().toArray().length, "other B 4");

for (var i = 0; i < 10; i++) {
    print("* C " + i);
    assert.eq(7, s2.getDB("test").foo.find().toArray().length, "other B " + i);
}

s.stop();
})();
