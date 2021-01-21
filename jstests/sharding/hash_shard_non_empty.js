// Hash sharding on a non empty collection should not pre-split.

load("jstests/sharding/libs/find_chunks_util.js");

var s = new ShardingTest({name: jsTestName(), shards: 3, mongos: 1, verbose: 1});
var dbname = "test";
var coll = "foo";
var db = s.getDB(dbname);
db.adminCommand({enablesharding: dbname});
s.ensurePrimaryShard('test', s.shard1.shardName);

// for simplicity turn off balancer
s.stopBalancer();

db.getCollection(coll).insert({a: 1});

db.getCollection(coll).createIndex({a: "hashed"});
var res = db.adminCommand({shardcollection: dbname + "." + coll, key: {a: "hashed"}});
assert.eq(res.ok, 1, "shardcollection didn't work");
s.printShardingStatus();
var numChunks = findChunksUtil.countChunksForNs(s.config, "test.foo");
assert.eq(numChunks, 1, "sharding non-empty collection should not pre-split");

s.stop();
