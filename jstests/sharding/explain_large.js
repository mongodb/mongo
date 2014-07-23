// Make sure that the explain output from mongos does not grow too large.

// Setup a large sharded cluster.
var st = new ShardingTest({shards: 18});
st.stopBalancer();

var db = st.s.getDB("test");
var coll = db.getCollection("mongos_large_explain");
coll.drop();

// The shard key will be 'a'. There's also an index on 'b'.
coll.ensureIndex({a: 1});
coll.ensureIndex({b: 1});

// Enable sharding.
assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
db.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}});

// Pre-split the collection to ensure that all 18 shards have chunks. Explicitly
// move the new chunks to the proper shards since the balancer is disabled.
for (var i = 1; i <= 18; i++) {
    assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {a: i}}));

    var shardNum = i - 1;
    var shardName = "shard000" + shardNum;
    if (shardNum >= 10) {
        shardName = "shard00" + shardNum;
    }
    printjson(db.adminCommand({moveChunk: coll.getFullName(),
                               find: {a: i},
                               to: shardName}));
}

// Put data on each shard.
for (var i = 0; i < 18; i++) {
    coll.insert({_id: i, a: i});
}

printjson(sh.status());

// Make a large array of ids, to be used in a large $in query. The explain output
// for this query will be very large, and will have to be truncated.
var ids = [];
for (var i = 0; i < 215500; i++) {
    ids.push(new ObjectId());
}

// Make sure that the explain succeeds. The query on 'b' should be sent to all shards,
// which makes the explain grow very large.
var explain = coll.find({b: {$in: ids}}).explain(true);
assert.eq("ParallelSort", explain.clusteredType);

st.stop();
