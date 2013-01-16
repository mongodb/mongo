
load( "jstests/libs/fts.js" );

t = db.fts_partition_no_multikey;
t.drop();

t.ensureIndex( { x : 1, y : "text" } )

t.insert( { x : 5 , y : "this is fun" } );
assert.isnull( db.getLastError() );

t.insert( { x : [] , y : "this is fun" } );
assert( db.getLastError() );

t.insert( { x : [1] , y : "this is fun" } );
assert( db.getLastError() );

t.insert( { x : [1,2] , y : "this is fun" } );
assert( db.getLastError() );
