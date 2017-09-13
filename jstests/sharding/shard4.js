// shard4.js

s = new ShardingTest({name: "shard4", shards: 2, mongos: 2});

s2 = s._mongos[1];

s.adminCommand({enablesharding: "test"});
s.ensurePrimaryShard('test', 'shard0001');
s.adminCommand({shardcollection: "test.foo", key: {num: 1}});
if (s.configRS) {
    // Ensure that the second mongos will see the movePrimary
    s.configRS.awaitLastOpCommitted();
}

s.getDB("test").foo.save({num: 1});
s.getDB("test").foo.save({num: 2});
s.getDB("test").foo.save({num: 3});
s.getDB("test").foo.save({num: 4});
s.getDB("test").foo.save({num: 5});
s.getDB("test").foo.save({num: 6});
s.getDB("test").foo.save({num: 7});

assert.eq(7, s.getDB("test").foo.find().toArray().length, "normal A");
assert.eq(7, s2.getDB("test").foo.find().toArray().length, "other A");

s.adminCommand({split: "test.foo", middle: {num: 4}});
s.adminCommand({
    movechunk: "test.foo",
    find: {num: 3},
    to: s.getOther(s.getPrimaryShard("test")).name,
    _waitForDelete: true
});

assert(s._connections[0].getDB("test").foo.find().toArray().length > 0, "blah 1");
assert(s._connections[1].getDB("test").foo.find().toArray().length > 0, "blah 2");
assert.eq(7,
          s._connections[0].getDB("test").foo.find().toArray().length +
              s._connections[1].getDB("test").foo.find().toArray().length,
          "blah 3");

assert.eq(7, s.getDB("test").foo.find().toArray().length, "normal B");
assert.eq(7, s2.getDB("test").foo.find().toArray().length, "other B");

s.adminCommand({split: "test.foo", middle: {num: 2}});
s.printChunks();

print("* A");

assert.eq(7, s.getDB("test").foo.find().toArray().length, "normal B 1");
assert.eq(7, s2.getDB("test").foo.find().toArray().length, "other B 2");
print("* B");
assert.eq(7, s.getDB("test").foo.find().toArray().length, "normal B 3");
assert.eq(7, s2.getDB("test").foo.find().toArray().length, "other B 4");

for (var i = 0; i < 10; i++) {
    print("* C " + i);
    assert.eq(7, s2.getDB("test").foo.find().toArray().length, "other B " + i);
}

s.stop();
