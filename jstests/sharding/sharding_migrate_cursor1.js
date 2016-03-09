// SERVER-2068
(function() {

    var chunkSize = 25;

    var s = new ShardingTest(
        {name: "migrate_cursor1", shards: 2, mongos: 1, other: {chunkSize: chunkSize}});

    s.adminCommand({enablesharding: "test"});
    db = s.getDB("test");
    s.ensurePrimaryShard('test', 'shard0001');
    t = db.foo;

    bigString = "";
    stringSize = 1024;

    while (bigString.length < stringSize)
        bigString += "asdasdas";

    stringSize = bigString.length;
    docsPerChunk = Math.ceil((chunkSize * 1024 * 1024) / (stringSize - 12));
    numChunks = 5;
    numDocs = 20 * docsPerChunk;

    print("stringSize: " + stringSize + " docsPerChunk: " + docsPerChunk + " numDocs: " + numDocs);

    var bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        bulk.insert({_id: i, s: bigString});
    }
    assert.writeOK(bulk.execute());

    s.adminCommand({shardcollection: "test.foo", key: {_id: 1}});

    assert.lt(numChunks, s.config.chunks.find().count(), "initial 1");

    primary = s.getPrimaryShard("test").getDB("test").foo;
    secondaryName = s.getOther(primary.name);
    secondary = secondaryName.getDB("test").foo;

    assert.eq(numDocs, primary.count(), "initial 2");
    assert.eq(0, secondary.count(), "initial 3");
    assert.eq(numDocs, t.count(), "initial 4");

    x = primary.find({_id: {$lt: 500}}).batchSize(2);
    x.next();  // 1. Create an open cursor

    print("start moving chunks...");

    // 2. Move chunk from s0 to s1 without waiting for deletion.
    // Command returns, but the deletion on s0 will block due to the open cursor.
    s.adminCommand({moveChunk: "test.foo", find: {_id: 0}, to: secondaryName.name});

    // 3. Start second moveChunk command from s0 to s1.
    // This moveChunk should not observe the above deletion as a 'mod', transfer it to s1 and cause
    // deletion on s1.
    // This moveChunk will wait for deletion.
    join = startParallelShell(
        "db.x.insert( {x:1} ); db.adminCommand( { moveChunk : 'test.foo' , find : { _id : " +
        docsPerChunk * 3 + " } , to : '" + secondaryName.name + "', _waitForDelete: true } )");
    assert.soon(function() {
        return db.x.count() > 0;
    }, "XXX", 30000, 1);

    // 4. Close the cursor to enable chunk deletion.
    print("itcount: " + x.itcount());

    x = null;
    for (i = 0; i < 5; i++)
        gc();

    print("cursor should be gone");

    // 5. Waiting for the second moveChunk to finish its deletion.
    // Note the deletion for the first moveChunk may not be finished.
    join();

    // assert.soon( function(){ return numDocs == t.count(); } , "at end 1" )
    // 6. Check the total number of docs on both shards to make sure no doc is lost.
    // Use itcount() to ignore orphan docments.
    assert.eq(numDocs, t.find().itcount(), "at end 2");

    s.stop();

})();
