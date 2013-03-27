//
// Tests cleanup of sharded and unsharded cursors
//

var st = new ShardingTest({ shards : 2, mongos : 1, other : { separateConfig : true } });
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB( "admin" );
var config = mongos.getDB( "config" );
var shards = config.shards.find().toArray();
var coll = mongos.getCollection( "foo.bar" );
var collUnsharded = mongos.getCollection( "foo.baz" );

// Shard collection
printjson(admin.runCommand({ enableSharding : coll.getDB() + "" }));
printjson(admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }));
printjson(admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }));
printjson(admin.runCommand({ split : coll + "", middle : { _id : 0 } }));
printjson(admin.runCommand({ moveChunk : coll + "", find : { _id : 0 }, to : shards[1]._id }));

jsTest.log("Collection set up...");
st.printShardingStatus(true);

jsTest.log("Insert enough data to overwhelm a query batch.");

for (var i = -150; i < 150; i++) {
    coll.insert({ _id : i });
    collUnsharded.insert({ _id : i });
}
assert.eq(null, coll.getDB().getLastError());

jsTest.log("Open a cursor to a sharded and unsharded collection.");

var shardedCursor = coll.find();
assert.neq(null, shardedCursor.next());

var unshardedCursor = collUnsharded.find();
assert.neq(null, unshardedCursor.next());

jsTest.log("Check whether the cursor is registered in the cursor info.");

var cursorInfo = admin.runCommand({ cursorInfo : true });
printjson(cursorInfo);

assert.eq(cursorInfo.sharded, 1);
assert.eq(cursorInfo.refs, 1);

jsTest.log("End the cursors.");

shardedCursor.itcount();
unshardedCursor.itcount();

var cursorInfo = admin.runCommand({ cursorInfo : true });
printjson(cursorInfo);

assert.eq(cursorInfo.sharded, 0);
assert.eq(cursorInfo.refs, 0);

jsTest.log("DONE!");

st.stop();
