// features2.js

s = new ShardingTest( "features2" , 2 , 1 , 1 );
s.adminCommand( { enablesharding : "test" } );

a = s._connections[0].getDB( "test" );
b = s._connections[1].getDB( "test" );

db = s.getDB( "test" );

// ---- distinct ----

db.foo.save( { x : 1 } );
db.foo.save( { x : 2 } );
db.foo.save( { x : 3 } );

assert.eq( "1,2,3" , db.foo.distinct( "x" ) , "distinct 1" );
assert( a.foo.distinct("x").length == 3 || b.foo.distinct("x").length == 3 , "distinct 2" );
assert( a.foo.distinct("x").length == 0 || b.foo.distinct("x").length == 0 , "distinct 3" );

assert.eq( 1 , s.onNumShards( "foo" ) , "A1" );

s.shardGo( "foo" , { x : 1 } , { x : 2 } , { x : 3 } );

assert.eq( 2 , s.onNumShards( "foo" ) , "A2" );

assert.eq( "1,2,3" , db.foo.distinct( "x" ) , "distinct 4" );

// ----- delete ---

assert.eq( 3 , db.foo.count() , "D1" );

db.foo.remove( { x : 3 } );
assert.eq( 2 , db.foo.count() , "D2" );

db.foo.save( { x : 3 } );
assert.eq( 3 , db.foo.count() , "D3" );

db.foo.remove( { x : { $gt : 2 } } );
assert.eq( 2 , db.foo.count() , "D4" );

db.foo.remove( { x : { $gt : -1 } } );
assert.eq( 0 , db.foo.count() , "D5" );

db.foo.save( { x : 1 } );
db.foo.save( { x : 2 } );
db.foo.save( { x : 3 } );
assert.eq( 3 , db.foo.count() , "D6" );
db.foo.remove( {} );
assert.eq( 0 , db.foo.count() , "D7" );

// --- map/reduce



s.stop();
