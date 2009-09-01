// features1.js

s = new ShardingTest( "features1" , 2 , 1 , 1 );

s.adminCommand( { enablesharding : "test" } );

// ---- can't shard system namespaces ----

assert( ! s.admin.runCommand( { shardcollection : "test.system.blah" , key : { num : 1 } } ).ok , "shard system namespace" );

// ---- setup test.foo -----

s.adminCommand( { shardcollection : "test.foo" , key : { num : 1 } } );

db = s.getDB( "test" );

a = s._connections[0].getDB( "test" );
b = s._connections[1].getDB( "test" );

db.foo.ensureIndex( { y : 1 } );

s.adminCommand( { split : "test.foo" , middle : { num : 10 } } );
s.adminCommand( { movechunk : "test.foo" , find : { num : 20 } , to : s.getOther( s.getServer( "test" ) ).name } );

db.foo.save( { num : 5 } );
db.foo.save( { num : 15 } );

s.sync();

// ---- make sure shard key index is everywhere ----

assert.eq( 3 , a.foo.getIndexKeys().length , "a index 1" );
assert.eq( 3 , b.foo.getIndexKeys().length , "b index 1" );

// ---- make sure if you add an index it goes everywhere ------

db.foo.ensureIndex( { x : 1 } );

s.sync();

assert.eq( 4 , a.foo.getIndexKeys().length , "a index 2" );
assert.eq( 4 , b.foo.getIndexKeys().length , "b index 2" );

// ---- no unique indexes ------

db.foo.ensureIndex( { z : 1 } , true );

s.sync();

assert.eq( 4 , a.foo.getIndexKeys().length , "a index 3" );
assert.eq( 4 , b.foo.getIndexKeys().length , "b index 3" );



s.stop()
