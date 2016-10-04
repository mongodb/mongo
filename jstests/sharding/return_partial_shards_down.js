//
// Tests that zero results are correctly returned with returnPartial and shards down
//

var st = new ShardingTest({shards: 3, mongos: 1, other: {mongosOptions: {verbose: 2}}});

// Stop balancer, we're doing our own manual chunk distribution
st.stopBalancer();

var mongos = st.s;
var config = mongos.getDB("config");
var admin = mongos.getDB("admin");
var shards = config.shards.find().toArray();

for (var i = 0; i < shards.length; i++) {
    shards[i].conn = new Mongo(shards[i].host);
}

var collOneShard = mongos.getCollection("foo.collOneShard");
var collAllShards = mongos.getCollection("foo.collAllShards");

printjson(admin.runCommand({enableSharding: collOneShard.getDB() + ""}));
printjson(admin.runCommand({movePrimary: collOneShard.getDB() + "", to: shards[0]._id}));

printjson(admin.runCommand({shardCollection: collOneShard + "", key: {_id: 1}}));
printjson(admin.runCommand({shardCollection: collAllShards + "", key: {_id: 1}}));

// Split and move the "both shard" collection to both shards

printjson(admin.runCommand({split: collAllShards + "", middle: {_id: 0}}));
printjson(admin.runCommand({split: collAllShards + "", middle: {_id: 1000}}));
printjson(admin.runCommand({moveChunk: collAllShards + "", find: {_id: 0}, to: shards[1]._id}));
printjson(admin.runCommand({moveChunk: collAllShards + "", find: {_id: 1000}, to: shards[2]._id}));

// Collections are now distributed correctly
jsTest.log("Collections now distributed correctly.");
st.printShardingStatus();

var inserts = [{_id: -1}, {_id: 1}, {_id: 1000}];

collOneShard.insert(inserts);
assert.writeOK(collAllShards.insert(inserts));

var returnPartialFlag = 1 << 7;

jsTest.log("All shards up!");

assert.eq(3, collOneShard.find().itcount());
assert.eq(3, collAllShards.find().itcount());

assert.eq(3, collOneShard.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());
assert.eq(3, collAllShards.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());

jsTest.log("One shard down!");

MongoRunner.stopMongod(st.shard2);

jsTest.log("done.");

assert.eq(3, collOneShard.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());
assert.eq(2, collAllShards.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());

jsTest.log("Two shards down!");

MongoRunner.stopMongod(st.shard1);

jsTest.log("done.");

assert.eq(3, collOneShard.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());
assert.eq(1, collAllShards.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());

jsTest.log("All shards down!");

MongoRunner.stopMongod(st.shard0);

jsTest.log("done.");

assert.eq(0, collOneShard.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());
assert.eq(0, collAllShards.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());

jsTest.log("DONE!");

st.stop();
