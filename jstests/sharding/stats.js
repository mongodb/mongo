s = new ShardingTest( "stats" , 2 , 1 , 1 );
s.adminCommand( { enablesharding : "test" } );

a = s._connections[0].getDB( "test" );
b = s._connections[1].getDB( "test" );

db = s.getDB( "test" );

function numKeys(o){
    var num = 0;
    for (var x in o)
        num++;
    return num;
}

// ---------- load some data -----

// need collections sharded before and after main collection for proper test
s.adminCommand( { shardcollection : "test.aaa" , key : { _id : 1 } } );
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } ); // this collection is actually used
s.adminCommand( { shardcollection : "test.zzz" , key : { _id : 1 } } );


N = 10000;
s.adminCommand( { split : "test.foo" , middle : { _id : N/2 } } )
s.adminCommand( { moveChunk : "test.foo", find : { _id : 3 } ,to : s.getNonPrimaries( "test" )[0] } )

for ( i=0; i<N; i++ )
    db.foo.insert( { _id : i } )
db.getLastError();

x = db.foo.stats();
assert.eq( N , x.count , "coll total count expected" )
assert.eq( db.foo.count() , x.count , "coll total count match" )
assert.eq( 2 , x.nchunks , "coll chunk num" )
assert.eq( 2 , numKeys(x.shards) , "coll shard num" )
assert.eq( N / 2 , x.shards.shard0000.count , "coll count on shard0000 expected" )
assert.eq( N / 2 , x.shards.shard0001.count , "coll count on shard0001 expected" )
assert.eq( a.foo.count() , x.shards.shard0000.count , "coll count on shard0000 match" )
assert.eq( b.foo.count() , x.shards.shard0001.count , "coll count on shard0001 match" )


a_extras = a.stats().objects - a.foo.count(); // things like system.namespaces and system.indexes
b_extras = b.stats().objects - b.foo.count(); // things like system.namespaces and system.indexes
print("a_extras: " + a_extras);
print("b_extras: " + b_extras);

x = db.stats();

//dbstats uses Future::CommandResult so raw output uses connection strings not shard names
shards = Object.keySet(x.raw);

assert.eq( N + (a_extras + b_extras) , x.objects , "db total count expected" )
assert.eq( 2 , numKeys(x.raw) , "db shard num" )
assert.eq( (N / 2) + a_extras, x.raw[shards[0]].objects , "db count on shard0000 expected" )
assert.eq( (N / 2) + b_extras, x.raw[shards[1]].objects , "db count on shard0001 expected" )
assert.eq( a.stats().objects , x.raw[shards[0]].objects , "db count on shard0000 match" )
assert.eq( b.stats().objects , x.raw[shards[1]].objects , "db count on shard0001 match" )

s.stop()
