// SERVER-6118: support for sharded sorts
(function() {

var s = new ShardingTest({ name: "aggregation_sort1", shards: 2, mongos: 1, verbose: 0 });
s.stopBalancer();

s.adminCommand({ enablesharding:"test" });
s.ensurePrimaryShard('test', 'shard0001');
s.adminCommand({ shardcollection: "test.data", key:{ _id: 1 } });

var d = s.getDB( "test" );

// Insert _id values 0 - 99
var N = 100;

var bulkOp = d.data.initializeOrderedBulkOp();
for(var i = 0; i < N; ++i) {
    bulkOp.insert({ _id: i });
}
bulkOp.execute();

// Split the data into 3 chunks
s.adminCommand( { split:"test.data", middle:{ _id:33 } } );
s.adminCommand( { split:"test.data", middle:{ _id:66 } } );

// Migrate the middle chunk to another shard
s.adminCommand({ movechunk: "test.data",
                 find: { _id: 50 },
                 to: s.getOther(s.getServer("test")).name });

// Check that the results are in order.
var result = d.data.aggregate({ $sort: { _id: 1 } }).toArray();
printjson(result);

for(var i = 0; i < N; ++i) {
    assert.eq(i, result[i]._id);
}

s.stop()

})();
