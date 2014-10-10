var st = new ShardingTest({ shards: 2, other: { chunkSize: 1 }});
st.stopBalancer();

var adminDB = st.admin;
var configDB = st.config;
var coll = st.s.getDB( 'test' ).user;

adminDB.runCommand({ enableSharding: coll.getDB().getName() });
adminDB.runCommand({ shardCollection: coll.getFullName(), key: { x: 1 }});

var data = 'c';
for( var x = 0; x < 18; x++ ){
    data += data;
}

for( x = 0; x < 200; x++ ){
    coll.insert({ x: x, v: data });
}

var chunkDoc = configDB.chunks.findOne();
var chunkOwner = chunkDoc.shard;
var toShard = configDB.shards.findOne({ _id: { $ne: chunkOwner }})._id;
var cmd = { moveChunk: coll.getFullName(), find: chunkDoc.min, to: toShard };
var res = adminDB.runCommand( cmd );

jsTest.log( 'move result: ' + tojson( res ));

var shardedCursorWithTimeout = coll.find();
var shardedCursorWithNoTimeout = coll.find();
shardedCursorWithNoTimeout.addOption( DBQuery.Option.noTimeout );

// Query directly to mongod
var shardHost = configDB.shards.findOne({ _id: chunkOwner }).host;
var mongod = new Mongo( shardHost );
var shardColl = mongod.getCollection( coll.getFullName() );

var cursorWithTimeout = shardColl.find();
var cursorWithNoTimeout = shardColl.find();
cursorWithNoTimeout.addOption( DBQuery.Option.noTimeout );

shardedCursorWithTimeout.next();
shardedCursorWithNoTimeout.next();

cursorWithTimeout.next();
cursorWithNoTimeout.next();

// Cursor cleanup is 10 minutes, but give a 8 min allowance --
// NOTE: Due to inaccurate timing on non-Linux platforms, mongos tries
// to timeout after 10 minutes but in fact is 15+ minutes;
// SERVER-8381
sleep( 1000 * 60 * 17 );

assert.throws( function(){ shardedCursorWithTimeout.itcount(); } );
assert.throws( function(){ cursorWithTimeout.itcount(); } );

var freshShardedItCount = coll.find().itcount();
// +1 because we already advanced once
assert.eq( freshShardedItCount, shardedCursorWithNoTimeout.itcount() + 1 );

var freshItCount = shardColl.find().itcount();
assert.eq( freshItCount, cursorWithNoTimeout.itcount() + 1 );

st.stop();

