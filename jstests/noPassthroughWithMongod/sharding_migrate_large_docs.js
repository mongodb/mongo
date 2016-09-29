//
// Tests migration behavior of large documents
//

var st = new ShardingTest({shards: 2, mongos: 1});
st.disableAutoSplit();
st.stopBalancer();

var mongos = st.s0;
var coll = mongos.getCollection("foo.bar");
var admin = mongos.getDB("admin");
var shards = mongos.getCollection("config.shards").find().toArray();
var shardAdmin = st.shard0.getDB("admin");

assert(admin.runCommand({enableSharding: coll.getDB() + ""}).ok);
printjson(admin.runCommand({movePrimary: coll.getDB() + "", to: shards[0]._id}));
assert(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}).ok);
assert(admin.runCommand({split: coll + "", middle: {_id: 0}}).ok);

jsTest.log("Preparing large insert...");

var data1MB = "x";
while (data1MB.length < 1024 * 1024)
    data1MB += data1MB;

var data15MB = "";
for (var i = 0; i < 15; i++)
    data15MB += data1MB;

var data15PlusMB = data15MB;
for (var i = 0; i < 1023 * 1024; i++)
    data15PlusMB += "x";

print("~15MB object size is : " + Object.bsonsize({_id: 0, d: data15PlusMB}));

jsTest.log("Inserting docs of large and small sizes...");

// Two large docs next to each other
coll.insert({_id: -2, d: data15PlusMB});
coll.insert({_id: -1, d: data15PlusMB});

// Docs of assorted sizes
assert.writeOK(coll.insert({_id: 0, d: "x"}));
assert.writeOK(coll.insert({_id: 1, d: data15PlusMB}));
assert.writeOK(coll.insert({_id: 2, d: "x"}));
assert.writeOK(coll.insert({_id: 3, d: data15MB}));
assert.writeOK(coll.insert({_id: 4, d: "x"}));
assert.writeOK(coll.insert({_id: 5, d: data1MB}));
assert.writeOK(coll.insert({_id: 6, d: "x"}));

assert.eq(9, coll.find().itcount());

jsTest.log("Starting migration...");

assert(admin.runCommand({moveChunk: coll + "", find: {_id: 0}, to: shards[1]._id}).ok);
assert(admin.runCommand({moveChunk: coll + "", find: {_id: -1}, to: shards[1]._id}).ok);

// Ensure that the doc count is correct and that the mongos query path can handle docs near the 16MB
// user BSON size limit.
assert.eq(9, coll.find().itcount());

jsTest.log("DONE!");

st.stop();
