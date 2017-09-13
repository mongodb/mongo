function setupMoveChunkTest(st) {
    // Stop Balancer
    st.stopBalancer();

    var testdb = st.getDB("test");
    var testcoll = testdb.foo;

    st.adminCommand({enablesharding: "test"});
    st.ensurePrimaryShard('test', 'shard0001');
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
    assert.writeOK(bulk.execute());

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
}
