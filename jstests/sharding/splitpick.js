// splitpick.js

/**
* tests picking the middle to split on
*/

s = new ShardingTest( "splitpick" , 2 );

db = s.getDB( "test" );

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { a : 1 } } );

c = db.foo;

for ( var i=1; i<20; i++ ){
    c.save( { a : i } );
}
c.save( { a : 99 } );
db.getLastError();

function checkSplit( f, want , num ){
    x = s.admin.runCommand( { splitvalue : "test.foo" , find : { a : f } } );
    assert.eq( want, x.middle ? x.middle.a : null , "splitvalue " + num + " " + tojson( x ) );
}

checkSplit( 1 , 1 , "1" )
checkSplit( 3 , 1 , "2" )

s.adminCommand( { split : "test.foo" , find : { a : 1 } } );
checkSplit( 3 , 99 , "3" )
s.adminCommand( { split : "test.foo" , find : { a : 99 } } );

assert.eq( s.config.chunks.count() , 3 );
s.printChunks();

checkSplit( 50 , 10 , "4" )

s.stop();
