// Tests "stacking" multiple migration cleanup threads and their behavior when the collection
// changes
(function() {
    'use strict';

    // start up a new sharded cluster
    var st = new ShardingTest({shards: 2, merizos: 1});

    var merizos = st.s;
    var admin = merizos.getDB("admin");
    var coll = merizos.getCollection("foo.bar");

    // Enable sharding of the collection
    assert.commandWorked(merizos.adminCommand({enablesharding: coll.getDB() + ""}));
    st.ensurePrimaryShard(coll.getDB() + "", st.shard0.shardName);
    assert.commandWorked(merizos.adminCommand({shardcollection: coll + "", key: {_id: 1}}));

    var numChunks = 30;

    // Create a bunch of chunks
    for (var i = 0; i < numChunks; i++) {
        assert.commandWorked(merizos.adminCommand({split: coll + "", middle: {_id: i}}));
    }

    jsTest.log("Inserting a lot of small documents...");

    // Insert a lot of small documents to make multiple cursor batches
    var bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < 10 * 1000; i++) {
        bulk.insert({_id: i});
    }
    assert.writeOK(bulk.execute());

    jsTest.log("Opening a merizod cursor...");

    // Open a new cursor on the merizod
    var cursor = coll.find();
    var next = cursor.next();

    jsTest.log("Moving a bunch of chunks to stack cleanup...");

    // Move a bunch of chunks, but don't close the cursor so they stack.
    for (var i = 0; i < numChunks; i++) {
        assert.commandWorked(
            merizos.adminCommand({moveChunk: coll + "", find: {_id: i}, to: st.shard1.shardName}));
    }

    jsTest.log("Dropping and re-creating collection...");

    coll.drop();

    bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < numChunks; i++) {
        bulk.insert({_id: i});
    }
    assert.writeOK(bulk.execute());

    sleep(10 * 1000);

    jsTest.log("Checking that documents were not cleaned up...");

    for (var i = 0; i < numChunks; i++) {
        assert.neq(null, coll.findOne({_id: i}));
    }

    st.stop();

})();
