// Tests for $out with a non-existing target collection.
(function() {
    "use strict";

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB[jsTestName()];
    const mongosTargetColl = mongosDB[jsTestName() + "_out"];

    function testOut(sourceColl, targetColl, shardedSource) {
        jsTestLog("Testing $out to non-existent target collection (source collection sharded : " +
                  shardedSource + ").");
        sourceColl.drop();
        targetColl.drop();

        if (shardedSource) {
            st.shardColl(sourceColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());
        }

        for (let i = 0; i < 10; i++) {
            assert.commandWorked(sourceColl.insert({_id: i}));
        }

        // Test the behavior for each of the $out modes. Since the target collection does not exist,
        // the behavior should be identical.
        ["insertDocuments", "replaceDocuments", "replaceCollection"].forEach(mode => {
            targetColl.drop();
            sourceColl.aggregate([{$out: {to: targetColl.getName(), mode: mode}}]);
            assert.eq(10, targetColl.find().itcount());
        });

        // Test with legacy syntax, which should behave identical to mode "replaceCollection".
        targetColl.drop();
        sourceColl.aggregate([{$out: targetColl.getName()}]);
        assert.eq(10, targetColl.find().itcount());
    }

    //
    // Test with unsharded source collection to a non-existent target collection.
    //
    testOut(mongosColl, mongosTargetColl, false);

    //
    // Test with sharded source collection to a non-existent target collection.
    //
    testOut(mongosColl, mongosTargetColl, true);

    st.stop();
}());
