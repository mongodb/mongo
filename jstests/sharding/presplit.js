// presplit.js

// Starts a new sharding environment limiting the chunksize to 1MB. 
s = new ShardingTest( "presplit" , 2 , 2 , 1 , { chunksize : 1 } );

// Insert enough data in 'test.foo' to fill several chunks, if it was sharded.
bigString = "";
while ( bigString.length < 10000 ){
    bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";
}

db = s.getDB( "test" );
inserted = 0;
num = 0;
while ( inserted < ( 20 * 1024 * 1024 ) ){
    db.foo.insert( { _id : num++ , s : bigString } );
    inserted += bigString.length;
}
db.getLastError();

// Make sure that there's only one chunk holding all the data.
s.printChunks();
primary = s.getServer( "test" ).getDB( "test" );
assert.eq( 0 , s.config.chunks.count()  , "single chunk assertion" );
assert.eq( num , primary.foo.count() );

// Turn on sharding on the 'test.foo' collection
s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );

// Make sure the collection's original chunk got split 
s.printChunks();
assert.lt( 20 , s.config.chunks.count() , "many chunks assertion" );
assert.eq( num , primary.foo.count() );

s.printChangeLog();
s.stop();