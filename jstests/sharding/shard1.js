/**
* this tests some of the ground work
*/

s = new ShardingTest({name: "shard1", shards: 2});

db = s.getDB("test");
db.foo.insert({num: 1, name: "eliot"});
db.foo.insert({num: 2, name: "sara"});
db.foo.insert({num: -1, name: "joe"});
db.foo.ensureIndex({num: 1});
assert.eq(3, db.foo.find().length(), "A");

shardCommand = {
    shardcollection: "test.foo",
    key: {num: 1}
};

assert.throws(function() {
    s.adminCommand(shardCommand);
});

s.adminCommand({enablesharding: "test"});
s.ensurePrimaryShard('test', 'shard0001');
assert.eq(3, db.foo.find().length(), "after partitioning count failed");

s.adminCommand(shardCommand);

assert.throws(function() {
    s.adminCommand({shardCollection: 'test', key: {x: 1}});
});
assert.throws(function() {
    s.adminCommand({shardCollection: '.foo', key: {x: 1}});
});

var cconfig = s.config.collections.findOne({_id: "test.foo"});
assert(cconfig, "why no collection entry for test.foo");

delete cconfig.lastmod;
delete cconfig.dropped;
delete cconfig.lastmodEpoch;

assert.eq(cconfig, {_id: "test.foo", key: {num: 1}, unique: false}, "Sharded content mismatch");

s.config.collections.find().forEach(printjson);

assert.eq(1, s.config.chunks.count(), "num chunks A");
si = s.config.chunks.findOne();
assert(si);
assert.eq(si.ns, "test.foo");

assert.eq(3, db.foo.find().length(), "after sharding, no split count failed");

s.stop();
