// SERVER-6179: support for two $groups in sharded agg

// Set up a sharding test.
s = new ShardingTest( "aggregation_multiple_group", 2, 0, 2 );
s.adminCommand( { enablesharding:"test" } );
s.adminCommand( { shardcollection:"test.data", key:{ _id:1 } } );
s.stopBalancer()

d = s.getDB( "test" );

// Populate
for( i = 0; i < 100; ++i ) {
    d.data.insert( { _id:i, i:i%10 } )
}
d.getLastError();

// Split the data into 3 chunks.
s.adminCommand( { split:"test.data", middle:{ _id:33 } } );
s.adminCommand( { split:"test.data", middle:{ _id:66 } } );

// Migrate the middle chunk to another shard.
s.adminCommand( { movechunk:"test.data", find:{ _id:50 },
                to:s.getOther( s.getServer( "test" ) ).name } );

// Check that we get results rather than an error
result = d.data.aggregate({$group: {_id: '$_id', i: {$first: '$i'}}},
                          {$group: {_id: '$i', avg_id: {$avg: '$_id'}}},
                          {$sort: {_id: 1}}).result;
expected = [
    {
        "_id" : 0,
        "avg_id" : 45
    },
    {
        "_id" : 1,
        "avg_id" : 46
    },
    {
        "_id" : 2,
        "avg_id" : 47
    },
    {
        "_id" : 3,
        "avg_id" : 48
    },
    {
        "_id" : 4,
        "avg_id" : 49
    },
    {
        "_id" : 5,
        "avg_id" : 50
    },
    {
        "_id" : 6,
        "avg_id" : 51
    },
    {
        "_id" : 7,
        "avg_id" : 52
    },
    {
        "_id" : 8,
        "avg_id" : 53
    },
    {
        "_id" : 9,
        "avg_id" : 54
    }
];

assert.eq(result, expected);

s.stop();
