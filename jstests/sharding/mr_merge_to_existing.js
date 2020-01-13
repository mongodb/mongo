// Test that running mapReduce to a target collection which is empty on the primary shard does *not*
// drop and reshard it.
(function() {

    load("jstests/libs/fixture_helpers.js");

    var st = new ShardingTest({
        shards: 2,
        rs: {nodes: 2},
    });

    const dbName = "test";
    const collName = jsTestName();

    let mongosConn = st.s;
    assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    // Shard the test collection and split it into two chunks.
    st.shardColl(collName,
                 {_id: 1} /* Shard key */,
                 {_id: 2} /* Split at */,
                 {_id: 2} /* Move the chunk to its own shard */,
                 dbName,
                 true /* Wait until documents orphaned by the move get deleted */);

    // Seed the source collection.
    let sourceColl = mongosConn.getDB(dbName)[collName];
    assert.commandWorked(sourceColl.insert({key: 1}));
    assert.commandWorked(sourceColl.insert({key: 2}));
    assert.commandWorked(sourceColl.insert({key: 3}));
    assert.commandWorked(sourceColl.insert({key: 4}));

    // Shard the target collection.
    let mergeColl = mongosConn.getDB(dbName).mr_merge_out;
    st.shardColl("mr_merge_out",
                 {_id: 1} /* Shard key */,
                 {_id: 2} /* Split at */,
                 {_id: 2} /* Move the chunk containing {_id: 2} to its own shard */,
                 dbName);

    // Insert a single document to the target collection and ensure that it lives on the non-primary
    // shard.
    assert.commandWorked(mergeColl.insert({_id: 5, value: 1}));

    function map() {
        emit(this.key, 1);
    }
    function reduce(key, values) {
        return Array.sum(values);
    }

    // Run the mapReduce to merge to the existing sharded collection.
    assert.commandWorked(
        sourceColl.mapReduce(map, reduce, {out: {merge: mergeColl.getName(), sharded: true}}));

    // Verify that the previous document still exists in the target collection.
    assert.eq(mergeColl.find({_id: 5}).itcount(), 1);

    st.stop();
})();
