// splitpick.js

/**
* tests picking the middle to split on
*/

s = new ShardingTest( "splitpick" , 2 );

db = s.getDB( "test" );

s.adminCommand( { partition : "test" } );
s.adminCommand( { shard : "test.foo" , key : { a : 1 } } );

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

assert.eq( s.config.shard.count() , 3 );
print( s.config.shard.find().toArray().tojson( "\n" ) );

assert.eq( s.admin.runCommand( { splitvalue : "test.foo" , find : { a : 50 } } ).middle.a , 11 , "splitvalue 4 " );

s.stop();
