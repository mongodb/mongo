// Basic test of sharding with a hashed shard key
//  - Test basic migrations with moveChunk, using different chunk specification methods

load("jstests/sharding/libs/find_chunks_util.js");

var s = new ShardingTest({name: jsTestName(), shards: 3, mongos: 1, verbose: 1});
var dbname = "test";
var coll = "foo";
var ns = dbname + "." + coll;
var db = s.getDB(dbname);
var t = db.getCollection(coll);
db.adminCommand({enablesharding: dbname});
s.ensurePrimaryShard(dbname, s.shard1.shardName);

// for simplicity start by turning off balancer
s.stopBalancer();

// shard a fresh collection using a hashed shard key
t.drop();
var res = db.adminCommand({shardcollection: ns, key: {a: "hashed"}});
assert.gt(findChunksUtil.countChunksForNs(s.config, ns), 3);
assert.eq(res.ok, 1, "shardcollection didn't work");
s.printShardingStatus();

// insert stuff
var numitems = 1000;
for (i = 0; i < numitems; i++) {
    t.insert({a: i});
}
// check they all got inserted
assert.eq(t.find().count(), numitems, "count off after inserts");
printjson(t.find().explain());

// find a chunk that's not on s.shard0.shardName
var chunk = s.config.chunks.findOne({shard: {$ne: s.shard0.shardName}});
assert.neq(chunk, null, "all chunks on s.shard0.shardName!");
printjson(chunk);

// try to move the chunk using an invalid specification method. should fail.
var res = db.adminCommand(
    {movechunk: ns, find: {a: 0}, bounds: [chunk.min, chunk.max], to: s.shard0.shardName});
assert.eq(res.ok, 0, "moveChunk shouldn't work with invalid specification method");

// now move a chunk using the lower/upper bound method. should work.
var res = db.adminCommand({movechunk: ns, bounds: [chunk.min, chunk.max], to: s.shard0.shardName});
printjson(res);
assert.eq(res.ok, 1, "movechunk using lower/upper bound method didn't work ");

// check count still correct.
assert.eq(t.find().itcount(), numitems, "count off after migrate");
printjson(t.find().explain());

// move a chunk using the find method
var res = db.adminCommand({movechunk: ns, find: {a: 2}, to: s.shard2.shardName});
printjson(res);
assert.eq(res.ok, 1, "movechunk using find query didn't work");

// check counts still correct
assert.eq(t.find().itcount(), numitems, "count off after migrate");
printjson(t.find().explain());

s.stop();
