// Tests for $merge with an existing target collection.
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}, config: 1});

    const mongosDB = st.s0.getDB("source_db");
    const sourceColl = mongosDB["source_coll"];
    const outputCollSameDb = mongosDB[jsTestName() + "_merge"];

    function testMerge(sourceColl, targetColl, shardedSource, shardedTarget) {
        jsTestLog(`Testing $merge from ${sourceColl.getFullName()} ` +
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

        // Test whenMatched: "fail" to an existing collection with no documents.
        sourceColl.aggregate([{
            $merge: {
                into: {
                    db: targetColl.getDB().getName(),
                    coll: targetColl.getName(),
                },
                whenMatched: "fail",
                whenNotMatched: "insert"
            }
        }]);
        assert.eq(10, targetColl.find().itcount());

        // Test whenMatched: "fail" to an existing collection with unique key conflicts.
        assertErrorCode(sourceColl,
                        [{
                           $merge: {
                               into: {
                                   db: targetColl.getDB().getName(),
                                   coll: targetColl.getName(),
                               },
                               whenMatched: "fail",
                               whenNotMatched: "insert"
                           }
                        }],
                        ErrorCodes.DuplicateKey);

        // Test $merge to an existing collection with no documents.
        targetColl.remove({});
        sourceColl.aggregate([{
            $merge: {
                into: {
                    db: targetColl.getDB().getName(),
                    coll: targetColl.getName(),
                },
                whenMatched: "replace",
                whenNotMatched: "insert"
            }
        }]);
        assert.eq(10, targetColl.find().itcount());

        // Test $merge to an existing collection with documents that match the default "on" fields.
        // Re-run the previous aggregation, expecting it to succeed and overwrite the existing
        // documents.
        sourceColl.aggregate([{
            $merge: {
                into: {
                    db: targetColl.getDB().getName(),
                    coll: targetColl.getName(),
                },
                whenMatched: "replace",
                whenNotMatched: "insert"
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
            $merge: {
                into: {
                    db: targetColl.getDB().getName(),
                    coll: targetColl.getName(),
                },
                whenMatched: "fail",
                whenNotMatched: "insert"
            }
        }]);
        assert.eq(20, targetColl.find().itcount());

        // Legacy $out is only supported to the same database.
        if (sourceColl.getDB() === targetColl.getDB()) {
            if (shardedTarget) {
                // Test that legacy $out fails if the target collection is sharded.
                assertErrorCode(sourceColl, [{$out: targetColl.getName()}], 28769);
            } else {
                // Test that legacy $out will drop the target collection and replace with the
                // contents of the source collection.
                sourceColl.aggregate([{$out: targetColl.getName()}]);
                assert.eq(10, targetColl.find().itcount());
            }
        }
    }

    //
    // Tests for $merge where the output collection is in the same database as the source
    // collection.
    //

    // Test with unsharded source and sharded target collection.
    testMerge(sourceColl, outputCollSameDb, false, true);

    // Test with sharded source and sharded target collection.
    testMerge(sourceColl, outputCollSameDb, true, true);

    // Test with sharded source and unsharded target collection.
    testMerge(sourceColl, outputCollSameDb, true, false);

    // Test with unsharded source and unsharded target collection.
    testMerge(sourceColl, outputCollSameDb, false, false);

    //
    // Tests for $merge to a database that differs from the source collection's database.
    //
    const foreignDb = st.s0.getDB("foreign_db");
    const outputCollDiffDb = foreignDb["output_coll"];

    // Test with sharded source and sharded target collection.
    testMerge(sourceColl, outputCollDiffDb, true, true);

    // Test with unsharded source and unsharded target collection.
    testMerge(sourceColl, outputCollDiffDb, false, false);

    // Test with unsharded source and sharded target collection.
    testMerge(sourceColl, outputCollDiffDb, false, true);

    // Test with sharded source and unsharded target collection.
    testMerge(sourceColl, outputCollDiffDb, true, false);

    st.stop();
}());
