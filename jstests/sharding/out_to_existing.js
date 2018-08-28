// Tests for $out with an existing target collection.
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}, config: 1});

    const mongosDB = st.s0.getDB("source_db");
    const sourceColl = mongosDB["source_coll"];
    const outputCollSameDb = mongosDB[jsTestName() + "_out"];

    function testOut(sourceColl, targetColl, shardedSource, shardedTarget) {
        jsTestLog(`Testing $out from ${sourceColl.getFullName()} ` +
                  `(${shardedSource ? "sharded" : "unsharded"}) to ${targetColl.getFullName()} ` +
                  `(${shardedTarget ? "sharded" : "unsharded"})`);
        sourceColl.drop();
        targetColl.drop();
        assert.commandWorked(targetColl.runCommand("create"));

        if (shardedSource) {
            st.shardColl(sourceColl, {_id: 1}, {_id: 0}, {_id: 1}, sourceColl.getDB().getName());
        }

        if (shardedTarget) {
            st.shardColl(targetColl, {_id: 1}, {_id: 0}, {_id: 1}, targetColl.getDB().getName());
        }

        for (let i = -5; i < 5; i++) {
            assert.commandWorked(sourceColl.insert({_id: i}));
        }

        // Test mode "insertDocuments" to an existing collection with no documents.
        sourceColl.aggregate([{
            $out: {
                to: targetColl.getName(),
                db: targetColl.getDB().getName(),
                mode: "insertDocuments"
            }
        }]);
        assert.eq(10, targetColl.find().itcount());

        // Test mode "insertDocuments" to an existing collection with unique key conflicts.
        assertErrorCode(sourceColl,
                        [{
                           $out: {
                               to: targetColl.getName(),
                               db: targetColl.getDB().getName(),
                               mode: "insertDocuments"
                           }
                        }],
                        ErrorCodes.DuplicateKey);

        // Test mode "replaceDocuments" to an existing collection with no documents.
        targetColl.remove({});
        sourceColl.aggregate([{
            $out: {
                to: targetColl.getName(),
                db: targetColl.getDB().getName(),
                mode: "replaceDocuments"
            }
        }]);
        assert.eq(10, targetColl.find().itcount());

        // Test mode "replaceDocuments" to an existing collection with documents that match the
        // unique key. Re-run the previous aggregation, expecting it to succeed and overwrite the
        // existing documents because the mode is "replaceDocuments".
        sourceColl.aggregate([{
            $out: {
                to: targetColl.getName(),
                db: targetColl.getDB().getName(),
                mode: "replaceDocuments"
            }
        }]);
        assert.eq(10, targetColl.find().itcount());

        // Replace all documents in the target collection with documents that don't conflict with
        // the insert operations.
        assert.commandWorked(targetColl.remove({}));
        let bulk = targetColl.initializeUnorderedBulkOp();
        for (let i = -10; i < -5; ++i) {
            bulk.insert({_id: i});
        }
        for (let i = 6; i < 11; ++i) {
            bulk.insert({_id: i});
        }
        assert.commandWorked(bulk.execute());

        sourceColl.aggregate([{
            $out: {
                to: targetColl.getName(),
                db: targetColl.getDB().getName(),
                mode: "insertDocuments"
            }
        }]);
        assert.eq(20, targetColl.find().itcount());

        if (shardedTarget) {
            // Test that mode "replaceCollection" fails if the target collection is sharded.
            assertErrorCode(sourceColl,
                            [{
                               $out: {
                                   to: targetColl.getName(),
                                   db: targetColl.getDB().getName(),
                                   mode: "replaceCollection"
                               }
                            }],
                            28769);

            // If the target collection is in the same database as the source collection, test that
            // the legacy syntax fails.
            if (sourceColl.getDB() === targetColl.getDB()) {
                assertErrorCode(sourceColl, [{$out: targetColl.getName()}], 28769);
            }
        } else if (sourceColl.getDB() !== targetColl.getDB()) {
            // TODO (SERVER-36832): "replaceCollection" doesn't work in a sharded cluster if the
            // source and target database are different.
            assertErrorCode(sourceColl,
                            [{
                               $out: {
                                   to: targetColl.getName(),
                                   db: targetColl.getDB().getName(),
                                   mode: "replaceCollection"
                               }
                            }],
                            50939);
        } else {
            // Test that mode "replaceCollection" will drop the target collection and replace with
            // the contents of the $out.
            sourceColl.aggregate([{
                $out: {
                    to: targetColl.getName(),
                    db: targetColl.getDB().getName(),
                    mode: "replaceCollection"
                }
            }]);
            assert.eq(10, targetColl.find().itcount());

            // Legacy syntax should behave identical to mode "replaceCollection" if the target
            // collection is in the same database as the source collection.
            if (sourceColl.getDB() === targetColl.getDB()) {
                sourceColl.aggregate([{$out: targetColl.getName()}]);
                assert.eq(10, targetColl.find().itcount());
            }
        }
    }

    //
    // Tests for $out where the output collection is in the same database as the source collection.
    //

    // Test with unsharded source and sharded target collection.
    testOut(sourceColl, outputCollSameDb, false, true);

    // Test with sharded source and sharded target collection.
    testOut(sourceColl, outputCollSameDb, true, true);

    // Test with sharded source and unsharded target collection.
    testOut(sourceColl, outputCollSameDb, true, false);

    // Test with unsharded source and unsharded target collection.
    testOut(sourceColl, outputCollSameDb, false, false);

    //
    // Tests for $out to a database that differs from the source collection's database.
    //
    const foreignDb = st.s0.getDB("foreign_db");
    const outputCollDiffDb = foreignDb["output_coll"];

    // Test with sharded source and sharded target collection.
    testOut(sourceColl, outputCollDiffDb, true, true);

    // Test with unsharded source and unsharded target collection.
    testOut(sourceColl, outputCollDiffDb, false, false);

    // Test with unsharded source and sharded target collection.
    testOut(sourceColl, outputCollDiffDb, false, true);

    // Test with sharded source and unsharded target collection.
    testOut(sourceColl, outputCollDiffDb, true, false);

    st.stop();
}());
