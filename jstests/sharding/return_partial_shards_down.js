//
// Tests that zero results are correctly returned with returnPartial and shards down
//

var st = new ShardingTest({shards: 3, bongos: 1, other: {bongosOptions: {verbose: 2}}});

// Stop balancer, we're doing our own manual chunk distribution
st.stopBalancer();

var bongos = st.s;
var config = bongos.getDB("config");
var admin = bongos.getDB("admin");
var shards = config.shards.find().toArray();

for (var i = 0; i < shards.length; i++) {
    shards[i].conn = new Bongo(shards[i].host);
}

var collOneShard = bongos.getCollection("foo.collOneShard");
var collAllShards = bongos.getCollection("foo.collAllShards");

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

BongoRunner.stopBongod(st.shard2);

jsTest.log("done.");

assert.eq(3, collOneShard.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());
assert.eq(2, collAllShards.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());

jsTest.log("Two shards down!");

BongoRunner.stopBongod(st.shard1);

jsTest.log("done.");

assert.eq(3, collOneShard.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());
assert.eq(1, collAllShards.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());

jsTest.log("All shards down!");

BongoRunner.stopBongod(st.shard0);

jsTest.log("done.");

assert.eq(0, collOneShard.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());
assert.eq(0, collAllShards.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());

jsTest.log("DONE!");

st.stop();
