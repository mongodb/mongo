function setupMoveChunkTest(shardOptions) {
    var st = new ShardingTest({
        shards: 2,
        mongos: 1,
        other: {
            chunkSize: 1,
            shardOptions: shardOptions,
        }
    });

    // Stop Balancer
    st.stopBalancer();

    var testdb = st.getDB("test");
    var testcoll = testdb.foo;

    st.adminCommand({enablesharding: "test"});
    st.ensurePrimaryShard('test', st.shard1.shardName);
    st.adminCommand({shardcollection: "test.foo", key: {_id: 1}});

    var str = "";
    while (str.length < 10000) {
        str += "asdasdsdasdasdasdas";
    }

    var data = 0;
    var num = 0;

    // Insert till you get to 10MB of data
    var bulk = testcoll.initializeUnorderedBulkOp();
    while (data < (1024 * 1024 * 10)) {
        bulk.insert({_id: num++, s: str});
        data += str.length;
    }
    assert.commandWorked(bulk.execute());

    // Make sure there are chunks to move
    for (var i = 0; i < 10; ++i) {
        assert.commandWorked(st.splitFind("test.foo", {_id: i}));
    }

    var stats = st.chunkCounts("foo");
    var to = "";
    for (shard in stats) {
        if (stats[shard] == 0) {
            to = shard;
            break;
        }
    }

    var result = st.adminCommand({
        movechunk: "test.foo",
        find: {_id: 1},
        to: to,
        _waitForDelete: true
    });  // some tests need this...
    assert(result, "movechunk failed: " + tojson(result));
    return st;
}
