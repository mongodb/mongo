
s = new ShardingTest( "shard_existing" , 2 /* numShards */, 1 /* verboseLevel */, 1 /* numMongos */, { chunksize : 1 } )

db = s.getDB( "test" )

stringSize = 10000

// we want a lot of data, so lets make a string to cheat :)
bigString = "";
while ( bigString.length < stringSize )
    bigString += "this is a big string. ";

dataSize = 20 * 1024 * 1024;

numToInsert = dataSize / stringSize 
print( "numToInsert: " + numToInsert )

for ( i=0; i<(dataSize/stringSize); i++ ) {
    db.data.insert( { _id : i , s : bigString } )
}

db.getLastError();

assert.lt( dataSize , db.data.stats().size )

s.adminCommand( { enablesharding : "test" } );
res = s.adminCommand( { shardcollection : "test.data" , key : { _id : 1 } } );
printjson( res );

assert.eq( 40 , s.config.chunks.find().itcount() , "not right number of chunks" );


s.stop();
