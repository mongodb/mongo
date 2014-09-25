//
// Tests that merging chunks does not prevent cluster from doing other metadata ops
//

var options = { separateConfig : true, shardOptions : { verbose : 0 } };

var st = new ShardingTest({ shards : 2, mongos : 2, other : options });
st.stopBalancer();

var mongos = st.s0;
var staleMongos = st.s1;
var admin = mongos.getDB( "admin" );
var shards = mongos.getCollection( "config.shards" ).find().toArray();
var coll = mongos.getCollection( "foo.bar" );

assert( admin.runCommand({ enableSharding : coll.getDB() + "" }).ok );
printjson( admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }) );
assert( admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }).ok );

st.printShardingStatus();

// Split and merge the first chunk repeatedly
jsTest.log( "Splitting and merging repeatedly..." );

for ( var i = 0; i < 5; i++ ) {
    assert( admin.runCommand({ split : coll + "", middle : { _id : i } }).ok );
    assert( admin.runCommand({ mergeChunks : coll + "", 
                               bounds : [{ _id : MinKey }, { _id : MaxKey }] }).ok );
    printjson( mongos.getDB("config").chunks.find().toArray() );
}

// Move the first chunk to the other shard
jsTest.log( "Moving to another shard..." );

assert( admin.runCommand({ moveChunk : coll + "", find : { _id : 0 }, to : shards[1]._id }).ok );

// Split and merge the chunk repeatedly
jsTest.log( "Splitting and merging repeatedly (again)..." );

for ( var i = 0; i < 5; i++ ) {
    assert( admin.runCommand({ split : coll + "", middle : { _id : i } }).ok );
    assert( admin.runCommand({ mergeChunks : coll + "", 
                               bounds : [{ _id : MinKey }, { _id : MaxKey }] }).ok );
    printjson( mongos.getDB("config").chunks.find().toArray() );
}

// Move the chunk back to the original shard
jsTest.log( "Moving to original shard..." );

assert( admin.runCommand({ moveChunk : coll + "", find : { _id : 0 }, to : shards[0]._id }).ok );

st.printShardingStatus();

jsTest.log( "DONE!" );

st.stop();

