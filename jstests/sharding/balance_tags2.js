// Test balancing all chunks to one shard by tagging the full shard-key range on that collection
var s = new ShardingTest({ name: "balance_tags2",
                           shards: 3,
                           mongos: 1,
                           other: { chunkSize: 1,
                                    enableBalancer : true } });

s.adminCommand({ enablesharding: "test" });
s.ensurePrimaryShard('test', 'shard0001');

var db = s.getDB("test");

var bulk = db.foo.initializeUnorderedBulkOp();
for (i = 0; i < 21; i++) {
    bulk.insert({ _id: i, x: i });
}
assert.writeOK(bulk.execute());

sh.shardCollection("test.foo", { _id : 1 });

sh.stopBalancer();

for (i = 0; i < 20; i++) {
    sh.splitAt("test.foo", {_id : i});
}

sh.startBalancer();

s.printShardingStatus(true);

// Wait for the initial balance to happen
assert.soon(function() {
                var counts = s.chunkCounts("foo");
                printjson(counts);
                return counts["shard0000"] == 7 &&
                       counts["shard0001"] == 7 &&
                       counts["shard0002"] == 7;
            },
            "balance 1 didn't happen",
            1000 * 60 * 10,
            1000);

// Tag one shard
sh.addShardTag("shard0000" , "a");
assert.eq([ "a" ] , s.config.shards.findOne({ _id : "shard0000" }).tags);

// Tag the whole collection (ns) to one shard
sh.addTagRange("test.foo", { _id : MinKey }, { _id : MaxKey }, "a");

// Wait for things to move to that one shard
s.printShardingStatus(true);

assert.soon(function() {
                var counts = s.chunkCounts("foo");
                printjson(counts);
                return counts["shard0001"] == 0 &&
                       counts["shard0002"] == 0;
            },
            "balance 2 didn't happen",
            1000 * 60 * 10,
            1000);

s.printShardingStatus(true);

s.stop();
