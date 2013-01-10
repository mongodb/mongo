/**
 * Tests whether the WBL gets reset without subsequent events.
 */

var st = new ShardingTest({ shards : 2, mongos : 2, other : { mongosOptions : { verbose : 5 } } });

st.stopBalancer();

var mongos = st.s0;
var staleMongos = st.s1;
var admin = mongos.getDB("admin");
var config = mongos.getDB("config");
var shards = config.shards.find().toArray();
var coll = mongos.getCollection("foo.bar");

jsTest.log("Sharding collection...");

printjson(admin.runCommand({ enableSharding : coll.getDB() + "" }));
printjson(admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }));
printjson(admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }));
printjson(admin.runCommand({ split : coll + "", middle : { _id : 0 } }));
printjson(admin.runCommand({ moveChunk : coll + "", find : { _id : 0 }, to : shards[1]._id }));

jsTest.log("Collection now sharded...");
st.printShardingStatus();

jsTest.log("Making mongos stale...");

coll.insert({ _id : 0 });
coll.getDB().getLastErrorObj();

// Make sure the stale mongos knows about the collection at the original version
assert.neq(null, staleMongos.getCollection(coll + "").findOne());

printjson(admin.runCommand({ moveChunk : coll + "", find : { _id : 0 }, to : shards[0]._id, _waitForDelete : true }));
printjson(admin.runCommand({ moveChunk : coll + "", find : { _id : 0 }, to : shards[1]._id, _waitForDelete : true }));

jsTest.log("Running a stale insert...");

staleMongos.getCollection(coll + "").insert({ _id : 0, dup : "key" });

jsTest.log("Getting initial GLE result...");

printjson(staleMongos.getDB(coll.getDB() + "").getLastErrorObj());
printjson(staleMongos.getDB(coll.getDB() + "").getLastErrorObj());
st.printShardingStatus();

jsTest.log("Performing insert op on the same shard...");

staleMongos.getCollection(coll + "").insert({ _id : 1, key : "isOk" })

jsTest.log("Getting GLE result...");

printjson(staleMongos.getDB(coll.getDB() + "").getLastErrorObj());
assert.eq(null, staleMongos.getDB(coll.getDB() + "").getLastError());

jsTest.log("DONE!");

st.stop();




