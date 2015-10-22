// Test balancing all chunks off of one shard
var st = new ShardingTest({ name: "balance_tags1",
                            shards: 3,
                            mongos: 1,
                            verbose: 1,
                            other: { chunkSize: 1,
                                     enableBalancer : true } });

st.adminCommand({ enablesharding: "test" });
st.ensurePrimaryShard('test', 'shard0001');

var db = st.getDB("test");

var bulk = db.foo.initializeUnorderedBulkOp();
for (i = 0; i < 21; i++) {
    bulk.insert({ _id: i, x: i });
}
assert.writeOK(bulk.execute());

assert.commandWorked(st.s.adminCommand({ shardCollection: 'test.foo', key: { _id : 1 } }));

st.stopBalancer();

for (i = 0; i < 20; i++) {
    st.adminCommand({ split : "test.foo", middle : { _id : i } });
}

st.startBalancer();

st.printShardingStatus();

// Wait for the initial balance to happen
assert.soon(function() {
                var counts = st.chunkCounts("foo");
                printjson(counts);
                return counts["shard0000"] == 7 &&
                       counts["shard0001"] == 7 &&
                       counts["shard0002"] == 7;
            },
            "balance 1 didn't happen",
            1000 * 60 * 10,
            1000);

// Quick test of some shell helpers and setting up state
sh.addShardTag("shard0000", "a");
assert.eq([ "a" ] , st.config.shards.findOne({ _id : "shard0000" }).tags);

sh.addShardTag("shard0000", "b");
assert.eq([ "a" , "b" ], st.config.shards.findOne({ _id : "shard0000" }).tags);

sh.removeShardTag("shard0000", "b");
assert.eq([ "a" ], st.config.shards.findOne( { _id : "shard0000" } ).tags);

sh.addShardTag("shard0001" , "a");
sh.addTagRange("test.foo" , { _id : -1 } , { _id : 1000 } , "a");

st.printShardingStatus();

// At this point, everything should drain off shard 2, which does not have the tag
assert.soon(function() {
                var counts = st.chunkCounts("foo");
                printjson(counts);
                return counts["shard0002"] == 0;
            },
            "balance 2 didn't happen",
            1000 * 60 * 10 , 1000);

st.printShardingStatus();

st.stop();
