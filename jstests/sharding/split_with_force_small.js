//
// Tests autosplit locations with force : true, for small collections
//

var options = { separateConfig : true, 
                chunksize : 1, // MB
                mongosOptions : { noAutoSplit : "" }
              };

var st = new ShardingTest({ shards : 1, mongos : 1, other : options });
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB( "admin" );
var config = mongos.getDB( "config" );
var shardAdmin = st.shard0.getDB( "admin" );
var coll = mongos.getCollection( "foo.bar" );

assert( admin.runCommand({ enableSharding : coll.getDB() + "" }).ok );
assert( admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : 0 } }).ok );

jsTest.log( "Insert a bunch of data into the low chunk of a collection," +
            " to prevent relying on stats." );

var data128k = "x";
for ( var i = 0; i < 7; i++ ) data128k += data128k;

for ( var i = 0; i < 1024; i++ ) {
    coll.insert({ _id : -(i + 1) });
}
assert.eq( null, coll.getDB().getLastError() );

jsTest.log( "Insert 32 docs into the high chunk of a collection" );

for ( var i = 0; i < 32; i++ ) {
    coll.insert({ _id : i });
}
assert.eq( null, coll.getDB().getLastError() );

jsTest.log( "Split off MaxKey chunk..." );

assert( admin.runCommand({ split : coll + "", middle : { _id : 32 } }).ok );

jsTest.log( "Keep splitting chunk multiple times..." );

st.printShardingStatus();

for ( var i = 0; i < 5; i++ ) {
    assert( admin.runCommand({ split : coll + "", find : { _id : 0 } }).ok );
    st.printShardingStatus();
}

// Make sure we can't split further than 5 (2^5) times
assert( !admin.runCommand({ split : coll + "", find : { _id : 0 } }).ok );

var chunks = config.chunks.find({ 'min._id' : { $gte : 0, $lt : 32 } }).sort({ min : 1 }).toArray();
printjson( chunks );

// Make sure the chunks grow by 2x (except the first)
var nextSize = 1;
for ( var i = 0; i < chunks.size; i++ ) {
    assert.eq( coll.count({ _id : { $gte : chunks[i].min._id, $lt : chunks[i].max._id } }), 
               nextSize );
    if ( i != 0 ) nextSize += nextSize;
}

st.stop();
