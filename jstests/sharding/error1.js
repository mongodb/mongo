
s = new ShardingTest( "error1" , 2 , 1 , 1 );
s.adminCommand( { enablesharding : "test" } );

// ---- simple getLastError ----

db = s.getDB( "test" );
db.foo.insert( { _id : 1 } );
assert.isnull( db.getLastError() , "gle 1" );
db.foo.insert( { _id : 1 } );
assert( db.getLastError() , "gle21" );


// ----
s.stop();
