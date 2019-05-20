// Tests that a $merge stage does not force a pipeline to split into a "shards part" and a "merging
// part" if no other stage in the pipeline would force such a split.
(function() {
    "use strict";

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    const mongosDB = st.s.getDB("test_db");

    const inColl = mongosDB["inColl"];
    // Two different output collections will be sharded by different keys.
    const outCollById = mongosDB["outCollById"];
    const outCollBySK = mongosDB["outCollBySK"];
    st.shardColl(outCollById, {_id: 1}, {_id: 500}, {_id: 500}, mongosDB.getName());
    st.shardColl(outCollBySK, {sk: 1}, {sk: 500}, {sk: 500}, mongosDB.getName());

    const numDocs = 1000;

    // Shard the input collection.
    st.shardColl(inColl, {_id: 1}, {_id: 500}, {_id: 500}, mongosDB.getName());

    // Insert some data to the input collection.
    const bulk = inColl.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({_id: i, sk: numDocs - i});
    }
    assert.commandWorked(bulk.execute());

    function assertOutRunsOnShards(explain) {
        assert(explain.hasOwnProperty("splitPipeline"), tojson(explain));
        assert(explain.splitPipeline.hasOwnProperty("shardsPart"), tojson(explain));
        assert.eq(
            explain.splitPipeline.shardsPart.filter(stage => stage.hasOwnProperty("$merge")).length,
            1,
            tojson(explain));
        assert(explain.splitPipeline.hasOwnProperty("mergerPart"), tojson(explain));
        assert.eq([], explain.splitPipeline.mergerPart, tojson(explain));
    }

    // Test that a simple $merge can run in parallel. Note that we still expect a 'splitPipeline' in
    // the explain output, but the merging half should be empty to indicate that the entire thing is
    // executing in parallel on the shards.
    let explain = inColl.explain().aggregate(
        [{$merge: {into: outCollById.getName(), whenMatched: "fail", whenNotMatched: "insert"}}]);
    assertOutRunsOnShards(explain);
    // Actually execute the pipeline and make sure it works as expected.
    assert.eq(outCollById.find().itcount(), 0);
    inColl.aggregate(
        [{$merge: {into: outCollById.getName(), whenMatched: "fail", whenNotMatched: "insert"}}]);
    assert.eq(outCollById.find().itcount(), numDocs);

    // Test the same thing but in a pipeline where the output collection's shard key differs from
    // the input collection's.
    explain = inColl.explain().aggregate(
        [{$merge: {into: outCollBySK.getName(), whenMatched: "fail", whenNotMatched: "insert"}}]);
    assertOutRunsOnShards(explain);
    // Again, test that execution works as expected.
    assert.eq(outCollBySK.find().itcount(), 0);
    inColl.aggregate(
        [{$merge: {into: outCollBySK.getName(), whenMatched: "fail", whenNotMatched: "insert"}}]);
    assert.eq(outCollBySK.find().itcount(), numDocs);

    st.stop();
}());
