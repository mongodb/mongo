// multi_mongos.js

// setup sharding with two mongos, s1 and s2
s1 = new ShardingTest( "multi_mongos1" , 2 , 1 , 2 );
s2 = s1._mongos[1];

s1.adminCommand( { enablesharding : "test" } );
s1.adminCommand( { shardcollection : "test.foo" , key : { num : 1 } } );

s1.config.databases.find().forEach( printjson )

viaS1 = s1.getDB( "test" ).foo;
viaS2 = s2.getDB( "test" ).foo;

primary = s1.getServer( "test" ).getDB( "test" ).foo;
secondary = s1.getOther( primary.name ).getDB( "test" ).foo;

N = 4;
for (i=1; i<=N; i++) {
    viaS1.save( { num : i } );
}

// initial checks

// both mongos see all elements
assert.eq( N , viaS1.find().toArray().length , "normal A" );
assert.eq( N , viaS2.find().toArray().length , "other A" );

// all elements are in one of the shards
assert.eq( N , primary.count() , "p1" )
assert.eq( 0 , secondary.count() , "s1" )
assert.eq( 1 , s1.onNumShards( "foo" ) , "on 1 shards" );

// 
// STEP 1 (builds a bit of context so there should probably not be a step 2 in this same test)
//   where we try to issue a move chunk from a mongos that's stale
//   followed by a split on a valid chunk, albeit one with not the highest lastmod

s1.stopBalancer()

// split in [Minkey->1), [1->N), [N,Maxkey)
s1.adminCommand( { split : "test.foo" , middle : { num : 1 } } );
s1.adminCommand( { split : "test.foo" , middle : { num : N } } );

// s2 is now stale w.r.t boundaires around { num: 1 }
res = s2.getDB( "admin" ).runCommand( { movechunk : "test.foo" , find : { num : 1 } , to : s1.getOther( s1.getServer( "test" ) ).name, _waitForDelete : true } );
assert.eq( 0 , res.ok , "a move with stale boundaries should not have succeeded" + tojson(res) ); 

// s2 must have reloaded as a result of a failed move; retrying should work
res = s2.getDB( "admin" ).runCommand( { movechunk : "test.foo" , find : { num : 1 } , to : s1.getOther( s1.getServer( "test" ) ).name, _waitForDelete : true } );
assert.eq( 1 , res.ok , "mongos did not reload after a failed migrate" + tojson(res) );

// s1 is not stale about the boundaries of [MinKey->1) 
// but we'll try to split a chunk whose lastmod.major was not touched by the previous move
// in 1.6, that chunk would be with [Minkey->1) (where { num: -1 } falls)
// after 1.6, it would be with [N->Maxkey] (where { num: N+1 } falls)
// s.printShardingStatus()
res = s1.getDB( "admin" ).runCommand( { split : "test.foo" , middle : { num : N+1 } } ); // replace with { num: -1 } instead in 1.6
assert.eq( 1, res.ok , "split over accurate boudaries should have succeeded" + tojson(res) );

s1.setBalancer( true )

// { num : 4 } is on primary
// { num : 1 , 2 , 3 } are on secondary
assert.eq( 1 , primary.find().toArray().length , "wrong count on primary" );
assert.eq( 3 , secondary.find().toArray().length , "wrong count on secondary" );
assert.eq( N , primary.find().itcount() + secondary.find().itcount() , "wrong total count" )

assert.eq( N , viaS1.find().toArray().length , "normal B" );
assert.eq( N , viaS2.find().toArray().length , "other B" );

printjson( primary._db._adminCommand( "shardingState" ) );


s1.stop();
