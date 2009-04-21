// auto1.js

s = new ShardingTest( "auto1" , 2 , 1 , 1 );

s.adminCommand( { partition : "test" } );
s.adminCommand( { shard : "test.foo" , key : { num : 1 } } );

bigString = "";
while ( bigString.length < 1024 * 50 )
    bigString += "asocsancdnsjfnsdnfsjdhfasdfasdfasdfnsadofnsadlkfnsaldknfsad";

db = s.getDB( "test" )
coll = db.foo;

var i=0;

for ( ; i<500; i++ ){
    coll.save( { num : i , s : bigString } );
}

s.adminCommand( "connpoolsync" );

primary = s.getServer( "test" ).getDB( "test" );

assert.eq( 1 , s.config.shard.count() );
assert.eq( 500 , primary.foo.count() );

print( "datasize: " + tojson( s.getServer( "test" ).getDB( "admin" ).runCommand( { datasize : "test.foo" } ) ) );

for ( ; i<800; i++ ){
    coll.save( { num : i , s : bigString } );
}

assert.eq( 1 , s.config.shard.count() );

for ( ; i<1500; i++ ){
    coll.save( { num : i , s : bigString } );
}

assert.eq( 3 , s.config.shard.count() , "shard didn't split A " );
print( s.config.shard.find().toArray().tojson( "\n" ) );

for ( ; i<3000; i++ ){
    coll.save( { num : i , s : bigString } );
}

assert.eq( 4 , s.config.shard.count() , "shard didn't split B " );
print( s.config.shard.find().toArray().tojson( "\n" ) );


s.stop();
