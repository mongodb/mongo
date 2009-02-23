/**
* test very basic sharding
*/

s = new ShardingTest( "shard2" , 2 );

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

s.adminCommand( "connpoolsync" );

assert.eq( 3 , s.getServer( "test" ).getDB( "test" ).foo.find().length() , "not right directly to db A" );
assert.eq( 3 , db.foo.find().length() );

primary = s.getServer( "test" ).getDB( "test" );
seconday = s.getOther( primary ).getDB( "test" );

assert.eq( 3 , primary.foo.find().length() , "primary wrong B" );
assert.eq( 0 , seconday.foo.find().length() , "seconday wrong C" );

// at this point we have 2 shard on 1 server

assert.throws( function(){ s.adminCommand( { moveshard : "test.foo" , find : { num : 1 } , to : primary.getMongo().name } ); } );
assert.throws( function(){ s.adminCommand( { moveshard : "test.foo" , find : { num : 1 } , to : "adasd" } ) } );

// test move shard

s.adminCommand( { moveshard : "test.foo" , find : { num : 1 } , to : seconday.getMongo().name } );
assert.eq( 1 , primary.foo.find().length() );
assert.eq( 2 , seconday.foo.find().length() );

assert.eq( 1 , s.config.sharding.count() );
shard = s.config.sharding.findOne();
assert.eq( 2 , shard.shards.length );
assert.neq( shard.shards[0].server , shard.shards[1].server , "servers should not be the same after the move" );

// test inserts go to right server/shard

db.foo.save( { num : 3 , name : "bob" } );
s.adminCommand( "connpoolsync" );
assert.eq( 1 , primary.foo.find().length() , "after move insert go wrong place?" );
assert.eq( 3 , seconday.foo.find().length() , "after move insert go wrong place?" );

db.foo.save( { num : -2 , name : "funny man" } );
s.adminCommand( "connpoolsync" );
assert.eq( 2 , primary.foo.find().length() , "after move insert go wrong place?" );
assert.eq( 3 , seconday.foo.find().length() , "after move insert go wrong place?" );


db.foo.save( { num : 0 , name : "funny man" } );
s.adminCommand( "connpoolsync" );
assert.eq( 2 , primary.foo.find().length() , "boundary A" );
assert.eq( 4 , seconday.foo.find().length() , "boundary B" );

// findOne
assert.eq( "eliot" , db.foo.findOne( { num : 1 } ).name );
assert.eq( "funny man" , db.foo.findOne( { num : -2 } ).name );

// TODO: getAll
//assert.eq( 3 , db.foo.find().length() );

// TODO: sort by num

// TODO: sory by name

// TODO: add wrong data to primary and make sure doesn't appear in collated results

s.stop();
