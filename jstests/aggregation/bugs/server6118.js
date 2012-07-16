// SERVER-6118: support for sharded sorts

// Set up a sharding test.
s = new ShardingTest( "aggregation_sort1", 2, 0, 2 );
s.adminCommand( { enablesharding:"test" } );
s.adminCommand( { shardcollection:"test.data", key:{ _id:1 } } );

// Test does it's own balancing.
s.stopBalancer();

d = s.getDB( "test" );

// Insert _id values 0 - 99.
N = 100;
for( i = 0; i < N; ++i ) {
    d.data.insert( { _id:i } )
}
d.getLastError();

// Split the data into 3 chunks.
s.adminCommand( { split:"test.data", middle:{ _id:33 } } );
s.adminCommand( { split:"test.data", middle:{ _id:66 } } );

// Migrate the middle chunk to another shard.
s.adminCommand( { movechunk:"test.data", find:{ _id:50 },
                to:s.getOther( s.getServer( "test" ) ).name } );

// Check that the results are in order.
result = d.data.aggregate( { $sort: { _id:1 } } ).result;
printjson(result);
for( i = 0; i < N; ++i ) {
    assert.eq( i, result[ i ]._id );
}

s.stop()
