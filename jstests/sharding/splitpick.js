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

assert.eq( s.admin.runCommand( { splitvalue : "test.foo" , find : { a : 1 } } ).middle.a , 1 , "splitvalue 1" );
assert.eq( s.admin.runCommand( { splitvalue : "test.foo" , find : { a : 3 } } ).middle.a , 1 , "splitvalue 2" );

s.adminCommand( { split : "test.foo" , find : { a : 1 } } );
assert.eq( s.admin.runCommand( { splitvalue : "test.foo" , find : { a : 3 } } ).middle.a , 99 , "splitvalue 3" );
s.adminCommand( { split : "test.foo" , find : { a : 99 } } );

assert.eq( s.config.chunks.count() , 3 );
s.printChunks();

assert.eq( s.admin.runCommand( { splitvalue : "test.foo" , find : { a : 50 } } ).middle.a , 10 , "splitvalue 4 " );

s.stop();
