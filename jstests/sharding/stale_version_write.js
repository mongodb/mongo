// Tests whether a reset sharding version triggers errors

jsTest.log("Starting sharded cluster...");

var st = new ShardingTest({shards: 1, mongos: 2, verbose: 2});

st.stopBalancer();

var mongosA = st.s0;
var mongosB = st.s1;

jsTest.log("Adding new collections...");

var collA = mongosA.getCollection(jsTestName() + ".coll");
assert.writeOK(collA.insert({hello: "world"}));

var collB = mongosB.getCollection("" + collA);
assert.writeOK(collB.insert({hello: "world"}));

jsTest.log("Enabling sharding...");

printjson(mongosA.getDB("admin").runCommand({enableSharding: "" + collA.getDB()}));
printjson(mongosA.getDB("admin").runCommand({shardCollection: "" + collA, key: {_id: 1}}));

// MongoD doesn't know about the config shard version *until* MongoS tells it
collA.findOne();

jsTest.log("Trigger shard version mismatch...");

assert.writeOK(collB.insert({goodbye: "world"}));

print("Inserted...");

assert.eq(3, collA.find().itcount());
assert.eq(3, collB.find().itcount());

st.stop();
