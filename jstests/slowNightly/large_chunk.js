// Where we test operations dealing with large chunks

// Starts a new sharding environment limiting the chunksize to 1GB (highest value allowed).
// Note that early splitting will start with a 1/4 of max size currently.  
s = new ShardingTest( "large_chunk" , 2 , 2 , 1 , { chunksize : 1024 } );

// take the balancer out of the equation
s.config.settings.update( { _id: "balancer" }, { $set : { stopped: true } } , true );
s.config.settings.find().forEach( printjson )
db = s.getDB( "test" );

//
// Step 1 - Test moving a large chunk
//

bigString = ""
while ( bigString.length < 10000 )
    bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";

inserted = 0;
num = 0;
while ( inserted < ( 400 * 1024 * 1024 ) ){
    db.foo.insert( { _id : num++ , s : bigString } );
    inserted += bigString.length;
}
db.getLastError();

// Turn on sharding on the 'test.foo' collection and generate a large chunk
s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );

assert.eq( 1 , s.config.chunks.count() , "step 1 - need one large chunk" );

primary = s.getServer( "test" ).getDB( "test" );
secondary = s.getOther( primary ).getDB( "test" );

// Make sure that we don't move that chunk if it goes past what we consider the maximum chunk size
print("Checkpoint 1a")
max = 200 * 1024 * 1024;
moveChunkCmd = { movechunk : "test.foo" , find : { _id : 1 } , to : secondary.getMongo().name , maxChunkSizeBytes : max };
assert.throws( function() { s.adminCommand( moveChunkCmd ); } );

// Move the chunk
print("checkpoint 1b");
before = s.config.chunks.find().toArray();
s.adminCommand( { movechunk : "test.foo" , find : { _id : 1 } , to : secondary.getMongo().name } );
after = s.config.chunks.find().toArray();
assert.neq( before[0].shard , after[0].shard , "move chunk did not work" );

s.config.changelog.find().forEach( printjson )

s.stop();
