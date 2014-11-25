// Tests "stacking" multiple migration cleanup threads and their behavior when the collection changes

// start up a new sharded cluster
var st = new ShardingTest({ shards : 2, mongos : 1 });

// stop balancer since we want manual control for this
st.stopBalancer();

var mongos = st.s;
var admin = mongos.getDB("admin");
var shards = mongos.getDB("config").shards.find().toArray();
var coll = mongos.getCollection("foo.bar");

// Enable sharding of the collection
printjson(mongos.adminCommand({ enablesharding : coll.getDB() + "" }));
printjson(mongos.adminCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }));
printjson(mongos.adminCommand({ shardcollection : coll + "", key: { _id : 1 } }));

var numChunks = 30;

// Create a bunch of chunks
for (var i = 0; i < numChunks; i++) {
    printjson(mongos.adminCommand({ split : coll + "", middle : { _id : i } }))
}

jsTest.log("Inserting a lot of small documents...")

// Insert a lot of small documents to make multiple cursor batches
for (var i = 0; i < 10 * 1000; i++) {
    coll.insert({ _id : i })
}
assert.eq(null, coll.getDB().getLastError());

jsTest.log("Opening a mongod cursor...");

// Open a new cursor on the mongod
var cursor = coll.find();
var next = cursor.next();

jsTest.log("Moving a bunch of chunks to stack cleanup...")

// Move a bunch of chunks, but don't close the cursor so they stack.
for (var i = 0; i < numChunks; i++) {
    printjson(mongos.adminCommand({ moveChunk : coll + "", find : { _id : i }, to : shards[1]._id }))
}

jsTest.log("Dropping and re-creating collection...")

coll.drop()
for (var i = 0; i < numChunks; i++) {
    coll.insert({ _id : i })
}
assert.eq(null, coll.getDB().getLastError());

sleep(10 * 1000);

jsTest.log("Checking that documents were not cleaned up...")

for (var i = 0; i < numChunks; i++) {
    assert.neq(null, coll.findOne({ _id : i }))   
}

jsTest.log("DONE!")

st.stop();


