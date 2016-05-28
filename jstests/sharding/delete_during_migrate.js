// Test migrating a big chunk while deletions are happening within that chunk.
// Test is slightly non-deterministic, since removes could happen before migrate
// starts. Protect against that by making chunk very large.

// start up a new sharded cluster
var st = new ShardingTest({shards: 2, mongos: 1});
// Balancer is by default stopped, thus we have manual control

var dbname = "testDB";
var coll = "foo";
var ns = dbname + "." + coll;
var s = st.s0;
var t = s.getDB(dbname).getCollection(coll);

s.adminCommand({enablesharding: dbname});
st.ensurePrimaryShard(dbname, 'shard0001');

// Create fresh collection with lots of docs
t.drop();
var bulk = t.initializeUnorderedBulkOp();
for (var i = 0; i < 200000; i++) {
    bulk.insert({a: i});
}
assert.writeOK(bulk.execute());

// enable sharding of the collection. Only 1 chunk.
t.ensureIndex({a: 1});
s.adminCommand({shardcollection: ns, key: {a: 1}});

// start a parallel shell that deletes things
startMongoProgramNoConnect("mongo",
                           "--host",
                           getHostName(),
                           "--port",
                           st.s0.port,
                           "--eval",
                           "db." + coll + ".remove({});",
                           dbname);

// migrate while deletions are happening
var moveResult =
    s.adminCommand({moveChunk: ns, find: {a: 1}, to: st.getOther(st.getPrimaryShard(dbname)).name});
// check if migration worked
assert(moveResult.ok, "migration didn't work while doing deletes");

st.stop();
