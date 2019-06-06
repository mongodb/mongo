// Tests that a $merge stage does not force a pipeline to split into a "shards part" and a "merging
// part" if no other stage in the pipeline would force such a split.
(function() {
    "use strict";

    load("jstests/aggregation/extras/merge_helpers.js");  // For withEachMergeMode.

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    const mongosDB = st.s.getDB("test_db");

    const inColl = mongosDB["inColl"];
    // Two different output collections will be sharded by different keys.
    const outCollById = mongosDB["outCollById"];
    const outCollBySK = mongosDB["outCollBySK"];
    st.shardColl(outCollById, {_id: 1}, {_id: 500}, {_id: 500}, mongosDB.getName());
    st.shardColl(outCollBySK, {sk: 1}, {sk: 500}, {sk: 500}, mongosDB.getName());
    const numDocs = 1000;

    function insertData(coll) {
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < numDocs; i++) {
            bulk.insert({_id: i, sk: numDocs - i});
        }
        assert.commandWorked(bulk.execute());
    }

    // Shard the input collection.
    st.shardColl(inColl, {_id: 1}, {_id: 500}, {_id: 500}, mongosDB.getName());

    // Insert some data to the input collection.
    insertData(inColl);

    function assertMergeRunsOnShards(explain) {
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

    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        assert.commandWorked(outCollById.remove({}));
        assert.commandWorked(outCollBySK.remove({}));

        let explain = inColl.explain().aggregate([{
            $merge: {
                into: outCollById.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        }]);
        assertMergeRunsOnShards(explain);
        assert.eq(outCollById.find().itcount(), 0);
        // We expect the test to succeed for all $merge modes. However, the 'whenNotMatched: fail'
        // mode will cause the test to fail if the source collection has a document without a match
        // in the target collection. Similarly 'whenNotMatched: discard' will fail the assertion
        // below for the expected number of document in target collection. So we populate the target
        // collection with the same documents as in the source.
        if (whenNotMatchedMode == "fail" || whenNotMatchedMode == "discard") {
            insertData(outCollById);
        }

        // Actually execute the pipeline and make sure it works as expected.
        assert.doesNotThrow(() => inColl.aggregate([{
            $merge: {
                into: outCollById.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        }]));
        assert.eq(outCollById.find().itcount(), numDocs);

        // Test the same thing but in a pipeline where the output collection's shard key differs
        // from the input collection's.
        explain = inColl.explain().aggregate([{
            $merge: {
                into: outCollBySK.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        }]);
        assertMergeRunsOnShards(explain);
        // Again, test that execution works as expected.
        assert.eq(outCollBySK.find().itcount(), 0);

        if (whenNotMatchedMode == "fail" || whenNotMatchedMode == "discard") {
            insertData(outCollBySK);
        }
        assert.doesNotThrow(() => inColl.aggregate([{
            $merge: {
                into: outCollBySK.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        }]));
        assert.eq(outCollBySK.find().itcount(), numDocs);
    });

    st.stop();
}());
