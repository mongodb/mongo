/**
* this tests some of the ground work
*/

s = new ShardingTest( "shard1" , 2 );

db = s.getDB( "test" );
db.foo.insert( { num : 1 , name : "eliot" } );
db.foo.insert( { num : 2 , name : "sara" } );
db.foo.insert( { num : -1 , name : "joe" } );
assert.eq( 3 , db.foo.count() );

assert( s.admin.runCommand( { partition : "test" } ).ok == 1 , "partition failed" );
//assert.eq( 3 , db.foo.count() , "after partitioning count failed" );

//s.admin.runCommand( { shard : "test.foo" , key : { num : 1 } } );
//assert.eq( 3 , db.foo.count() , "after sharding, no split count failed" );


s.stop();
