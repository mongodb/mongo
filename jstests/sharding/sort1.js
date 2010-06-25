
s = new ShardingTest( "sort1" , 2 , 0 , 2 )

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.data" , key : { num : 1 } } );

db = s.getDB( "test" );

for ( i=0; i<100; i++ ){
    db.data.insert( { _id : i , num : i } );
}
db.getLastError();

s.adminCommand( { split : "test.data" , middle : { num : 33 } } )
s.adminCommand( { split : "test.data" , middle : { num : 66 } } )

s.adminCommand( { movechunk : "test.data" , find : { num : 50 } , to : s.getOther( s.getServer( "test" ) ).name } );

assert.eq( 3 , s.config.chunks.find().itcount() , "A1" );

temp = s.config.chunks.find().sort( { min : 1 } ).toArray();
assert.eq( temp[0].shard , temp[2].shard , "A2" );
assert.neq( temp[0].shard , temp[1].shard , "A3" );

temp = db.data.find().sort( { num : 1 } ).toArray();
assert.eq( 100 , temp.length , "B1" );
for ( i=0; i<100; i++ ){
    assert.eq( i , temp[i].num , "B2" )
}

s.stop();
