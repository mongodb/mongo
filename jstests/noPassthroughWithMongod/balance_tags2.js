// test balancing all chunks to one shard by tagging the full shard-key range on that collection
s = new ShardingTest( "balance_tags2" , 3 , 1 , 1 ,
                        { sync:true, chunksize : 1 , nopreallocj : true }
                    )

s.config.settings.save({ _id: "balancer" });

db = s.getDB( "test" );

sh.enableSharding("test");
s.ensurePrimaryShard('test', 'shard0001');
var bulk = db.foo.initializeUnorderedBulkOp();
for (i = 0; i < 21; i++) {
    bulk.insert({ _id: i, x: i });
}
assert.writeOK(bulk.execute());

sh.shardCollection("test.foo" , { _id : 1 });
sh.stopBalancer();

for ( i=0; i<20; i++ )
    sh.splitAt("test.foo" , {_id : i});

sh.startBalancer();

// wait for everything to spread out
assert.soon( function() {
    counts = s.chunkCounts( "foo" );
    print("Chunk counts: " + tojson( counts ));
    return counts["shard0000"] == 7 &&
        counts["shard0001"] == 7 &&
        counts["shard0002"] == 7;
} , "balance 1 didn't happen" , 1000 * 60 * 10 , 1000 )

// tag one shard
sh.addShardTag( "shard0000" , "a" )
assert.eq( [ "a" ] , s.config.shards.findOne( { _id : "shard0000" } ).tags );

// tag the whole collection (ns) to one shard
sh.addTagRange( "test.foo" , { _id : MinKey } , { _id : MaxKey } , "a" )

// wait for things to move to that one shard
sh.status();
assert.soon( function() {
    counts = s.chunkCounts( "foo" );
    print("Chunk counts: " + tojson( counts ));
    return counts["shard0002"] == 0 && counts["shard0001"] == 0;
} , "balance 2 didn't happen" , 1000 * 60 * 10 , 1000 )

// print status and stop
printjson(sh.status());
s.stop();
