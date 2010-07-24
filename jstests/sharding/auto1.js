// auto1.js

s = new ShardingTest( "auto1" , 2 , 1 , 1 );

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { num : 1 } } );

bigString = "";
while ( bigString.length < 1024 * 50 )
    bigString += "asocsancdnsjfnsdnfsjdhfasdfasdfasdfnsadofnsadlkfnsaldknfsad";

db = s.getDB( "test" )
coll = db.foo;

var i=0;

for ( ; i<100; i++ ){
    coll.save( { num : i , s : bigString } );
}
db.getLastError();

primary = s.getServer( "test" ).getDB( "test" );

s.printChunks();
assert.eq( 1 , s.config.chunks.count() );
assert.eq( 100 , primary.foo.count() );

print( "datasize: " + tojson( s.getServer( "test" ).getDB( "admin" ).runCommand( { datasize : "test.foo" } ) ) );

for ( ; i<200; i++ ){
    coll.save( { num : i , s : bigString } );
}

s.printChunks()
assert.eq( 1 , s.config.chunks.count() );

for ( ; i<400; i++ ){
    coll.save( { num : i , s : bigString } );
}

s.printChunks();
assert.lte( 3 , s.config.chunks.count() , "shard didn't split A " );

for ( ; i<700; i++ ){
    coll.save( { num : i , s : bigString } );
}
db.getLastError();

s.printChunks();
assert.lte( 4 , s.config.chunks.count() , "shard didn't split B " );


s.stop();
