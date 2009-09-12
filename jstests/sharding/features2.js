// features2.js

s = new ShardingTest( "features2" , 2 , 1 , 1 );
s.adminCommand( { enablesharding : "test" } );

a = s._connections[0].getDB( "test" );
b = s._connections[1].getDB( "test" );

// ---- distinct ----

db = s.getDB( "test" );
db.foo.save( { x : 1 } );
db.foo.save( { x : 2 } );
db.foo.save( { x : 3 } );

assert.eq( "1,2,3" , db.foo.distinct( "x" ) , "distinct 1" );
assert( a.foo.distinct("x").length == 3 || b.foo.distinct("x").length == 3 , "distinct 2" );
assert( a.foo.distinct("x").length == 0 || b.foo.distinct("x").length == 0 , "distinct 3" );

s.adminCommand( { shardcollection : "test.foo" , key : { x : 1 } } );
s.adminCommand( { split : "test.foo" , middle : { x : 2 } } );
s.adminCommand( { movechunk : "test.foo" , find : { x : 3 } , to : s.getOther( s.getServer( "test" ) ).name } );

assert.eq( "1,2,3" , db.foo.distinct( "x" ) , "distinct 4" );

// -----
s.stop();
