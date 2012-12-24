// test to make sure that tag ranges get split

s = new ShardingTest( "tag_auto_split", 2, 0, 1, { nopreallocj : true } );

db = s.getDB( "test" );

s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );

assert.eq( 1, s.config.chunks.count() );

sh.addShardTag( "shard0000" , "a" )

sh.addTagRange( "test.foo" , { _id : 5 } , { _id : 10 } , "a" )
sh.addTagRange( "test.foo" , { _id : 10 } , { _id : 15 } , "b" )

assert.soon( function() {
    //printjson( sh.status() );
    return s.config.chunks.count() == 3;
}, "things didn't get split", 1000 * 60 * 10, 1000 );

printjson( sh.status() );

s.stop();

//test without full shard key on tags
s = new ShardingTest( "tag_auto_split2", 2, 0, 1, { nopreallocj : true } );

db = s.getDB( "test" );

s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1, a : 1 } } );

assert.eq( 1, s.config.chunks.count() );

sh.addShardTag( "shard0000" , "a" )

sh.addTagRange( "test.foo" , { _id : 5 } , { _id : 10 } , "a" )
sh.addTagRange( "test.foo" , { _id : 10 } , { _id : 15 } , "b" )

assert.soon( function() {
    //printjson( sh.status() );
    return s.config.chunks.count() == 3;
}, "things didn't get split", 1000 * 60 * 10, 1000 );

s.config.chunks.find().forEach(
    function(chunk){
        assert( chunk.min._id );
        assert( chunk.min.a );
    }
);

s.stop();
