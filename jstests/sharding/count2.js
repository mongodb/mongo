// count2.js

s1 = new ShardingTest( "count2" , 2 , 1 , 2 );
s2 = s1._mongos[1];

s1.adminCommand( { enablesharding: "test" } );
s1.adminCommand( { shardcollection: "test.foo" , key : { name : 1 } } );

db1 = s1.getDB( "test" ).foo;
db2 = s2.getDB( "test" ).foo;

assert.eq( 1, s1.config.chunks.count(), "sanity check A");

db1.save( { name : "aaa" } )
db1.save( { name : "bbb" } )
db1.save( { name : "ccc" } )
db1.save( { name : "ddd" } )
db1.save( { name : "eee" } )
db1.save( { name : "fff" } )

s1.adminCommand( { split : "test.foo" , middle : { name : "ddd" } } );

assert.eq( 3, db1.count( { name : { $gte: "aaa" , $lt: "ddd" } } ) , "initial count mongos1" );
assert.eq( 3, db2.count( { name : { $gte: "aaa" , $lt: "ddd" } } ) , "initial count mongos2" );

s1.adminCommand( { movechunk : "test.foo" , find : { name : "aaa" } , to : s1.getOther( s1.getServer( "test" ) ).name } );

assert.eq( 3, db1.count( { name : { $gte: "aaa" , $lt: "ddd" } } ) , "post count mongos1" );
// TOFIX
// The second mongos still thinks its shard mapping is valid and accepts a cound
//assert.eq( 3, db2.count( { name : { $gte: "aaa" , $lt: "ddd" } } ) , "post count mongos2" ); // ***** asserting *****

db2.findOne();

assert.eq( 3, db2.count( { name : { $gte: "aaa" , $lt: "ddd" } } ) );

s1.stop();