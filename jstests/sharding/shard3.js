// shard3.js

s = new ShardingTest( "shard3" , 2 , 50 , 2 );

s2 = s._mongos[1];

s.adminCommand( { partition : "test" } );
s.adminCommand( { shard : "test.foo" , key : { num : 1 } } );

s.getDB( "test" ).foo.save( { num : 1 } );
s.getDB( "test" ).foo.save( { num : 2 } );
s.getDB( "test" ).foo.save( { num : 3 } );

assert.eq( 3 , s.getDB( "test" ).foo.find().toArray().length , "normal A" );
assert.eq( 3 , s2.getDB( "test" ).foo.find().toArray().length , "other A" );

s.adminCommand( { split : "test.foo" , middle : { num : 2 } } );
s.adminCommand( { moveshard : "test.foo" , find : { num : 3 } , to : s.getOther( s.getServer( "test" ) ).name } );

assert( s._connections[0].getDB( "test" ).foo.find().toArray().length > 0 , "blah 1" );
assert( s._connections[1].getDB( "test" ).foo.find().toArray().length > 0 , "blah 2" );
assert.eq( 3 , s._connections[0].getDB( "test" ).foo.find().toArray().length + 
           s._connections[1].getDB( "test" ).foo.find().toArray().length  , "blah 3" );

assert.eq( 3 , s.getDB( "test" ).foo.find().toArray().length , "normal B" );
print( "HI" );
assert.eq( 3 , s2.getDB( "test" ).foo.find().toArray().length , "other B" );


s.stop();
