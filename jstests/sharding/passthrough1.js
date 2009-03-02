
s = new ShardingTest( "passthrough1" , 2 )

db = s.getDB( "test" );
db.foo.insert( { num : 1 , name : "eliot" } );
db.foo.insert( { num : 2 , name : "sara" } );
db.foo.insert( { num : -1 , name : "joe" } );
assert.eq( 3 , db.foo.find().length() );

s.stop();
