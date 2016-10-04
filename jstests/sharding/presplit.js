(function() {

    var s = new ShardingTest({name: "presplit", shards: 2, mongos: 1, other: {chunkSize: 1}});

    s.adminCommand({enablesharding: "test"});
    s.ensurePrimaryShard('test', 'shard0001');

    // Insert enough data in 'test.foo' to fill several chunks, if it was sharded.
    bigString = "";
    while (bigString.length < 10000) {
        bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";
    }

    db = s.getDB("test");
    inserted = 0;
    num = 0;
    var bulk = db.foo.initializeUnorderedBulkOp();
    while (inserted < (20 * 1024 * 1024)) {
        bulk.insert({_id: num++, s: bigString});
        inserted += bigString.length;
    }
    assert.writeOK(bulk.execute());

    // Make sure that there's only one chunk holding all the data.
    s.printChunks();
    primary = s.getPrimaryShard("test").getDB("test");
    assert.eq(0, s.config.chunks.count(), "single chunk assertion");
    assert.eq(num, primary.foo.count());

    s.adminCommand({shardcollection: "test.foo", key: {_id: 1}});

    // Make sure the collection's original chunk got split
    s.printChunks();
    assert.lt(20, s.config.chunks.count(), "many chunks assertion");
    assert.eq(num, primary.foo.count());

    s.printChangeLog();
    s.stop();

})();
