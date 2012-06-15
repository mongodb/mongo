
s = new ShardingTest( "balance_tags1" , 3 , 1 , 1 , { chunksize : 1 , nopreallocj : true } )
s.config.settings.update( { _id: "balancer" }, { $set : { stopped: false, _nosleep: true } } , true );

db = s.getDB( "test" );
for ( i=0; i<21; i++ ) {
    db.foo.insert( { _id : i , x : i } );
}
db.getLastError();

s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );

for ( i=0; i<20; i++ )
    s.adminCommand( { split : "test.foo" , middle : { _id : i } } );

db.printShardingStatus( true )

assert.soon( function() {
    counts = s.chunkCounts( "foo" );
    return counts["shard0000"] == 7 && 
        counts["shard0001"] == 7 &&
        counts["shard0002"] == 7;
} , "balance 1 didn't happen" , 1000 * 60 * 10 , 1000 )

s.config.shards.update( { _id : "shard0000" } , { $push : { tags : "a" } } );
s.config.shards.update( { _id : "shard0001" } , { $push : { tags : "a" } } );

s.config.tags.insert( { ns : "test.foo" , min : { _id : -1 } , max : { _id : 1000 } , tag : "a" } )

assert.soon( function() {
    counts = s.chunkCounts( "foo" );
    printjson( counts )
    return counts["shard0002"] == 0;
} , "balance 2 didn't happen" , 1000 * 60 * 10 , 1000 )


s.stop();



