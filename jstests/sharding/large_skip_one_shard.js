/**
 * Tests that a sharded query targeted to a single shard will use passed-in skip.
 */
var st = new ShardingTest({shards: 2, mongos: 1});

var mongos = st.s0;
var shards = mongos.getDB("config").shards.find().toArray();

var admin = mongos.getDB("admin");
var collSharded = mongos.getCollection("testdb.collSharded");
var collUnSharded = mongos.getCollection("testdb.collUnSharded");

// Set up a sharded and unsharded collection
assert(admin.runCommand({enableSharding: collSharded.getDB() + ""}).ok);
printjson(admin.runCommand({movePrimary: collSharded.getDB() + "", to: shards[0]._id}));
assert(admin.runCommand({shardCollection: collSharded + "", key: {_id: 1}}).ok);
assert(admin.runCommand({split: collSharded + "", middle: {_id: 0}}).ok);
assert(admin.runCommand({moveChunk: collSharded + "", find: {_id: 0}, to: shards[1]._id}).ok);

function testSelectWithSkip(coll) {
    for (var i = -100; i < 100; i++) {
        assert.writeOK(coll.insert({_id: i}));
    }

    // Run a query which only requires 5 results from a single shard
    var explain =
        coll.find({_id: {$gt: 1}}).sort({_id: 1}).skip(90).limit(5).explain("executionStats");

    assert.lt(explain.executionStats.nReturned, 90);
}

testSelectWithSkip(collSharded);
testSelectWithSkip(collUnSharded);

jsTest.log("DONE!");
st.stop();
