
s = new ShardingTest( "error1" , 2 , 1 , 1 );
s.adminCommand( { enablesharding : "test" } );

a = s._connections[0].getDB( "test" );
b = s._connections[1].getDB( "test" );

// ---- simple getLastError ----

db = s.getDB( "test" );
db.foo.insert( { _id : 1 } );
assert.isnull( db.getLastError() , "gle 1" );
db.foo.insert( { _id : 1 } );
assert( db.getLastError() , "gle21" );
assert( db.getLastError() , "gle22" );

// --- sharded getlasterror

s.adminCommand( { shardcollection : "test.foo2" , key : { num : 1 } } );

db.foo2.save( { _id : 1 , num : 5 } );
db.foo2.save( { _id : 2 , num : 10 } );
db.foo2.save( { _id : 3 , num : 15 } );
db.foo2.save( { _id : 4 , num : 20 } );

s.adminCommand( { split : "test.foo2" , middle : { num : 10 } } );
s.adminCommand( { movechunk : "test.foo2" , find : { num : 20 } , to : s.getOther( s.getServer( "test" ) ).name } );

print( "a: " + a.foo2.count() );
print( "b: " + b.foo2.count() );
assert( a.foo2.count() > 0 && a.foo2.count() < 4 , "se1" );
assert( b.foo2.count() > 0 && b.foo2.count() < 4 , "se2" );
assert.eq( 4 , db.foo2.count() , "se3" );

db.foo2.save( { _id : 5 , num : 25 } );
assert( ! db.getLastError() , "se3.5" );
s.sync();
assert.eq( 5 , db.foo2.count() , "se4" );



db.foo2.insert( { _id : 5 , num : 30 } );
assert( db.getLastError() , "se5" );
assert( db.getLastError() , "se6" );

assert.eq( 5 , db.foo2.count() , "se5" );


// assert in mongos
s.adminCommand( { shardcollection : "test.foo3" , key : { num : 1 } } );
assert.isnull(db.getLastError() , "gle C1" );

db.foo3.insert({}); //this fails with no shard key error
assert(db.getLastError() , "gle C2a" );
assert(db.getLastError() , "gle C2b" );

db.foo3.insert({num:1});
assert.isnull(db.getLastError() , "gle C3a" );

// ----
s.stop();
