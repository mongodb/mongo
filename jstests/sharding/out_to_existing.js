// Tests for $out with an existing target collection.
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB[jsTestName()];
    const mongosTargetColl = mongosDB[jsTestName() + "_out"];

    function testOut(sourceColl, targetColl, shardedSource, shardedTarget) {
        jsTestLog("Testing $out from source collection (sharded: " + shardedSource +
                  ") to target collection (sharded: " + shardedTarget + ")");
        sourceColl.drop();
        targetColl.drop();

        if (shardedSource) {
            st.shardColl(sourceColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());
        }

        if (shardedTarget) {
            st.shardColl(targetColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());
        }

        for (let i = 0; i < 10; i++) {
            assert.commandWorked(sourceColl.insert({_id: i}));
        }

        // Test mode "insertDocuments" to an existing collection with no documents.
        sourceColl.aggregate([{$out: {to: targetColl.getName(), mode: "insertDocuments"}}]);
        assert.eq(10, targetColl.find().itcount());

        // Test mode "insertDocuments" to an existing collection with unique key conflicts.
        assertErrorCode(sourceColl,
                        [{$out: {to: targetColl.getName(), mode: "insertDocuments"}}],
                        ErrorCodes.DuplicateKey);

        // Test mode "replaceDocuments" to an existing collection with no documents.
        targetColl.remove({});
        sourceColl.aggregate([{$out: {to: targetColl.getName(), mode: "replaceDocuments"}}]);
        assert.eq(10, targetColl.find().itcount());

        // Test mode "replaceDocuments" to an existing collection with documents that match the
        // unique key. Re-run the previous aggregation, expecting it to succeed and overwrite the
        // existing documents because the mode is "replaceDocuments".
        sourceColl.aggregate([{$out: {to: targetColl.getName(), mode: "replaceDocuments"}}]);
        assert.eq(10, targetColl.find().itcount());

        // Replace all documents in the target collection with documents that don't conflict with
        // the insert operations.
        targetColl.remove({});
        for (let i = 10; i < 20; i++) {
            assert.commandWorked(targetColl.insert({_id: i}));
        }

        sourceColl.aggregate([{$out: {to: targetColl.getName(), mode: "insertDocuments"}}]);
        assert.eq(20, targetColl.find().itcount());

        if (!shardedTarget) {
            // Test that mode "replaceCollection" will drop the target collection and replace with
            // the contents of the $out.
            sourceColl.aggregate([{$out: {to: targetColl.getName(), mode: "replaceCollection"}}]);
            assert.eq(10, targetColl.find().itcount());

            // Legacy syntax should behave identical to mode "replaceCollection".
            sourceColl.aggregate([{$out: targetColl.getName()}]);
            assert.eq(10, targetColl.find().itcount());
        } else {
            // Test that mode "replaceCollection" fails if the target collection is sharded.
            assertErrorCode(
                sourceColl, [{$out: {to: targetColl.getName(), mode: "replaceCollection"}}], 28769);

            assertErrorCode(sourceColl, [{$out: targetColl.getName()}], 28769);
        }
    }

    //
    // Test with unsharded source and sharded target collection.
    //
    testOut(mongosColl, mongosTargetColl, false, true);

    //
    // Test with sharded source and sharded target collection.
    //
    testOut(mongosColl, mongosTargetColl, true, true);

    //
    // Test with sharded source and unsharded target collection.
    //
    testOut(mongosColl, mongosTargetColl, true, false);

    //
    // Test with unsharded source and unsharded target collection.
    //
    testOut(mongosColl, mongosTargetColl, false, false);

    st.stop();
}());
