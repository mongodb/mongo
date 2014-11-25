//
// Tests cleanup of orphaned data in hashed sharded coll via the orphaned data cleanup command
//

var options = { separateConfig : true, shardOptions : { verbose : 2 } };

var st = new ShardingTest({ shards : 2, mongos : 1, other : options });
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB( "admin" );
var shards = mongos.getCollection( "config.shards" ).find().toArray();
var coll = mongos.getCollection( "foo.bar" );

assert( admin.runCommand({ enableSharding : coll.getDB() + "" }).ok );
printjson( admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }) );
assert( admin.runCommand({ shardCollection : coll + "", key : { _id : "hashed" } }).ok );

// Create two orphaned data holes, one bounded by min or max on each shard

assert( admin.runCommand({ split : coll + "", middle : { _id : NumberLong(-100) } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : NumberLong(-50) } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : NumberLong(50) } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : NumberLong(100) } }).ok );
assert( admin.runCommand({ moveChunk : coll + "", bounds : [{ _id : NumberLong(-100) },
                                                            { _id : NumberLong(-50) }],
                                                  to : shards[1]._id,
                                                  _waitForDelete : true }).ok );
assert( admin.runCommand({ moveChunk : coll + "", bounds : [{ _id : NumberLong(50) },
                                                            { _id : NumberLong(100) }],
                                                  to : shards[0]._id,
                                                  _waitForDelete : true }).ok );
st.printShardingStatus();

jsTest.log( "Inserting some docs on each shard, so 1/2 will be orphaned..." );

for ( var s = 0; s < 2; s++ ) {
    var shardColl = ( s == 0 ? st.shard0 : st.shard1 ).getCollection( coll + "" );
    for ( var i = 0; i < 100; i++ ) shardColl.insert({ _id : i });
    assert.eq( null, shardColl.getDB().getLastError() );
}

assert.eq( 200, st.shard0.getCollection( coll + "" ).find().itcount() +
                st.shard1.getCollection( coll + "" ).find().itcount() );
assert.eq( 100, coll.find().itcount() );

jsTest.log( "Cleaning up orphaned data in hashed coll..." );

for ( var s = 0; s < 2; s++ ) {
    var shardAdmin = ( s == 0 ? st.shard0 : st.shard1 ).getDB( "admin" );

    var result = shardAdmin.runCommand({ cleanupOrphaned : coll + "" });
    while ( result.ok && result.stoppedAtKey ) {
        printjson( result );
        result = shardAdmin.runCommand({ cleanupOrphaned : coll + "",
                                         startingFromKey : result.stoppedAtKey });
    }
    
    printjson( result );
    assert( result.ok );
}

assert.eq( 100, st.shard0.getCollection( coll + "" ).find().itcount() +
                st.shard1.getCollection( coll + "" ).find().itcount() );
assert.eq( 100, coll.find().itcount() );

jsTest.log( "DONE!" );

st.stop();
