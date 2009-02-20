/**
* test very basic sharding
*/

s = new ShardingTest( "shard2" , 2 , 5);

db = s.getDB( "test" );

s.adminCommand( { partition : "test" } );
s.adminCommand( { shard : "test.foo" , key : { num : 1 } } );
assert.eq( 1 , s.config.sharding.count()  , "sanity check 1" );
assert.eq( 1, s.config.sharding.findOne().shards.length );

s.adminCommand( { split : "test.foo" , find : { num : 0 } } );
assert.eq( 1 , s.config.sharding.count() );
shard = s.config.sharding.findOne();
assert.eq( 2 , shard.shards.length );
assert.eq( shard.shards[0].server , shard.shards[1].server , "server should be the same after a split" );


db.foo.save( { num : 1 , name : "eliot" } );
db.foo.save( { num : 2 , name : "sara" } );
db.foo.save( { num : -1 , name : "joe" } );

sleep( 100 ); // TODO: remove

assert.eq( 3 , s.getServer( "test" ).getDB( "test" ).foo.find().length() );

s.stop();
