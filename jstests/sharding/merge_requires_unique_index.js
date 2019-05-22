// Tests that the $merge stage enforces that the "on" fields argument can be used to uniquely
// identify documents by checking that there is a supporting unique, non-partial,
// collator-compatible index in the index catalog. This is meant to test sharding-related
// configurations that are not covered by the aggregation passthrough suites.
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");        // For assertErrorCode.
    load("jstests/aggregation/extras/out_helpers.js");  // For withEachMergeMode,
                                                        // assertFailsWithoutUniqueIndex,
                                                        // assertSucceedsWithExpectedUniqueIndex.

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}, config: 1});

    const mongosDB = st.s0.getDB("merge_requires_unique_index");
    const foreignDB = st.s0.getDB("merge_requires_unique_index_foreign");
    const sourceColl = mongosDB.source;
    let targetColl = mongosDB.target;
    sourceColl.drop();

    // Enable sharding on the test DB and ensure that shard0 is the primary.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Enable sharding on the foreign DB, except ensure that shard1 is the primary shard.
    assert.commandWorked(foreignDB.adminCommand({enableSharding: foreignDB.getName()}));
    st.ensurePrimaryShard(foreignDB.getName(), st.rs1.getURL());

    // Increase the log verbosity for sharding, in the hope of getting a clearer picture of the
    // cluster writer as part of BF-11106. This should be removed once BF-11106 is fixed.
    st.shard0.getDB("admin").setLogLevel(4, 'sharding');
    st.shard1.getDB("admin").setLogLevel(4, 'sharding');

    function resetTargetColl(shardKey, split) {
        targetColl.drop();
        // Shard the target collection, and set the unique flag to ensure that there's a unique
        // index on the shard key.
        assert.commandWorked(mongosDB.adminCommand(
            {shardCollection: targetColl.getFullName(), key: shardKey, unique: true}));
        assert.commandWorked(
            mongosDB.adminCommand({split: targetColl.getFullName(), middle: split}));
        assert.commandWorked(mongosDB.adminCommand(
            {moveChunk: targetColl.getFullName(), find: split, to: st.rs1.getURL()}));
    }

    function runOnFieldsTests(targetShardKey, targetSplit) {
        jsTestLog("Running unique key tests for target shard key " + tojson(targetShardKey));
        resetTargetColl(targetShardKey, targetSplit);

        // Not specifying "on" fields should always pass.
        assertSucceedsWithExpectedUniqueIndex({source: sourceColl, target: targetColl});

        // Since the target collection is sharded with a unique shard key, specifying "on" fields
        // that is equal to the shard key should be valid.
        assertSucceedsWithExpectedUniqueIndex(
            {source: sourceColl, target: targetColl, onFields: Object.keys(targetShardKey)});

        // Create a compound "on" fields consisting of the shard key and one additional field.
        let prefixPipeline = [{$addFields: {newField: 1}}];
        const indexSpec = Object.merge(targetShardKey, {newField: 1});

        // Expect the $merge to fail since we haven't created a unique index on the compound
        // "on" fields.
        assertFailsWithoutUniqueIndex({
            source: sourceColl,
            onFields: Object.keys(indexSpec),
            target: targetColl,
            prevStages: prefixPipeline
        });

        // Create the unique index and verify that the "on" fields is now valid.
        assert.commandWorked(targetColl.createIndex(indexSpec, {unique: true}));
        assertSucceedsWithExpectedUniqueIndex({
            source: sourceColl,
            target: targetColl,
            onFields: Object.keys(indexSpec),
            prevStages: prefixPipeline
        });

        // Create a non-unique index and make sure that doesn't work.
        assert.commandWorked(targetColl.dropIndex(indexSpec));
        assert.commandWorked(targetColl.createIndex(indexSpec));
        assertFailsWithoutUniqueIndex({
            source: sourceColl,
            onFields: Object.keys(indexSpec),
            target: targetColl,
            prevStages: prefixPipeline
        });

        // Test that a unique, partial index on the "on" fields cannot be used to satisfy the
        // requirement.
        resetTargetColl(targetShardKey, targetSplit);
        assert.commandWorked(targetColl.createIndex(
            indexSpec, {unique: true, partialFilterExpression: {a: {$gte: 2}}}));
        assertFailsWithoutUniqueIndex({
            source: sourceColl,
            onFields: Object.keys(indexSpec),
            target: targetColl,
            prevStages: prefixPipeline
        });

        // Test that a unique index on the "on" fields cannot be used to satisfy the requirement if
        // it has a different collation.
        resetTargetColl(targetShardKey, targetSplit);
        assert.commandWorked(
            targetColl.createIndex(indexSpec, {unique: true, collation: {locale: "en_US"}}));
        assertFailsWithoutUniqueIndex({
            source: sourceColl,
            onFields: Object.keys(indexSpec),
            target: targetColl,
            prevStages: prefixPipeline
        });
        assertFailsWithoutUniqueIndex({
            source: sourceColl,
            onFields: Object.keys(indexSpec),
            target: targetColl,
            options: {collation: {locale: "en"}},
            prevStages: prefixPipeline
        });
        assertFailsWithoutUniqueIndex({
            source: sourceColl,
            onFields: Object.keys(indexSpec),
            target: targetColl,
            options: {collation: {locale: "simple"}},
            prevStages: prefixPipeline
        });
        assertFailsWithoutUniqueIndex({
            source: sourceColl,
            onFields: Object.keys(indexSpec),
            target: targetColl,
            options: {collation: {locale: "en_US", strength: 1}},
            prevStages: prefixPipeline
        });
        assertSucceedsWithExpectedUniqueIndex({
            source: sourceColl,
            target: targetColl,
            onFields: Object.keys(indexSpec),
            options: {collation: {locale: "en_US"}},
            prevStages: prefixPipeline
        });

        // Test that a unique index with dotted field names can be used.
        resetTargetColl(targetShardKey, targetSplit);
        const dottedPathIndexSpec = Object.merge(targetShardKey, {"newField.subField": 1});
        assert.commandWorked(targetColl.createIndex(dottedPathIndexSpec, {unique: true}));

        // No longer a supporting index on the original compound "on" fields.
        assertFailsWithoutUniqueIndex({
            source: sourceColl,
            onFields: Object.keys(indexSpec),
            target: targetColl,
            prevStages: prefixPipeline
        });

        // Test that an embedded object matching the "on" fields is valid.
        prefixPipeline = [{$addFields: {"newField.subField": 5}}];
        assertSucceedsWithExpectedUniqueIndex({
            source: sourceColl,
            target: targetColl,
            onFields: Object.keys(dottedPathIndexSpec),
            prevStages: prefixPipeline
        });

        // Test that we cannot use arrays with a dotted path within a $merge.
        resetTargetColl(targetShardKey, targetSplit);
        assert.commandWorked(targetColl.createIndex(dottedPathIndexSpec, {unique: true}));
        withEachMergeMode(
            ({whenMatchedMode, whenNotMatchedMode}) => {
                assertErrorCode(
                    sourceColl,
                    [
                      {
                        $replaceRoot:
                            {newRoot: {$mergeObjects: ["$$ROOT", {newField: [{subField: 1}]}]}}
                      },
                      {
                        $merge: {
                            into: {
                                db: targetColl.getDB().getName(),
                                coll: targetColl.getName(),
                            },
                            whenMatched: whenMatchedMode,
                            whenNotMatched: whenNotMatchedMode,
                            on: Object.keys(dottedPathIndexSpec)
                        }
                      }
                    ],
                    51132);
            });

        // Test that a unique index that is multikey can still be used.
        resetTargetColl(targetShardKey, targetSplit);
        assert.commandWorked(targetColl.createIndex(dottedPathIndexSpec, {unique: true}));
        assert.commandWorked(targetColl.insert(
            Object.merge(targetShardKey, {newField: [{subField: "hi"}, {subField: "hello"}]})));
        assert.commandWorked(sourceColl.update(
            {}, {$set: {newField: {subField: "hi"}, proofOfUpdate: "PROOF"}}, {multi: true}));

        // If whenMatched is "replace" and whenNotMatched is "insert", expect the command to
        // fail if the "on" fields does not contain _id, since a replacement-style update will fail
        // if attempting to modify _id.
        if (dottedPathIndexSpec.hasOwnProperty("_id")) {
            assert.doesNotThrow(() => sourceColl.aggregate([{
                $merge: {
                    into: {
                        db: targetColl.getDB().getName(),
                        coll: targetColl.getName(),
                    },
                    whenMatched: "replace",
                    whenNotMatched: "insert",
                    on: Object.keys(dottedPathIndexSpec)
                }
            }]));
            assert.docEq(targetColl.findOne({"newField.subField": "hi", proofOfUpdate: "PROOF"},
                                            {"newField.subField": 1, proofOfUpdate: 1, _id: 0}),
                         {newField: {subField: "hi"}, proofOfUpdate: "PROOF"});
        } else {
            assertErrMsgContains(sourceColl,
                                 [{
                                    $merge: {
                                        into: {
                                            db: targetColl.getDB().getName(),
                                            coll: targetColl.getName(),
                                        },
                                        whenMatched: "replace",
                                        whenNotMatched: "insert",
                                        on: Object.keys(dottedPathIndexSpec)
                                    }
                                 }],
                                 ErrorCodes.ImmutableField,
                                 "did you attempt to modify the _id or the shard key?");

            assert.doesNotThrow(() => sourceColl.aggregate([
                {$project: {_id: 0}},
                {
                  $merge: {
                      into: {
                          db: targetColl.getDB().getName(),
                          coll: targetColl.getName(),
                      },
                      whenMatched: "replace",
                      whenNotMatched: "insert",
                      on: Object.keys(dottedPathIndexSpec)
                  }
                }
            ]));
            assert.docEq(targetColl.findOne({"newField.subField": "hi", proofOfUpdate: "PROOF"},
                                            {"newField.subField": 1, proofOfUpdate: 1, _id: 0}),
                         {newField: {subField: "hi"}, proofOfUpdate: "PROOF"});
        }
    }

    function testAgainstDB(targetDB) {
        targetColl = targetDB["target"];
        targetColl.drop();

        //
        // Test unsharded source and sharded target collections.
        //
        let targetShardKey = {_id: 1, a: 1, b: 1};
        let splitPoint = {_id: 0, a: 0, b: 0};
        sourceColl.drop();
        assert.commandWorked(sourceColl.insert([{a: 0, b: 0}, {a: 1, b: 1}]));
        runOnFieldsTests(targetShardKey, splitPoint);

        // Test with a shard key that does *not* include _id.
        targetShardKey = {a: 1, b: 1};
        splitPoint = {a: 0, b: 0};
        runOnFieldsTests(targetShardKey, splitPoint);

        //
        // Test both source and target collections as sharded.
        //
        targetShardKey = {_id: 1, a: 1, b: 1};
        splitPoint = {_id: 0, a: 0, b: 0};
        sourceColl.drop();
        st.shardColl(sourceColl.getName(), {a: 1}, {a: 0}, {a: 1}, mongosDB.getName());
        assert.commandWorked(sourceColl.insert([{a: 0, b: 0}, {a: 1, b: 1}]));
        runOnFieldsTests(targetShardKey, splitPoint);

        // Re-run the test with a shard key that does *not* include _id.
        targetShardKey = {a: 1, b: 1};
        splitPoint = {a: 0, b: 0};
        runOnFieldsTests(targetShardKey, splitPoint);
    }

    // First test $merge to the same database as the source.
    testAgainstDB(mongosDB);

    // Then test against a foreign database, with the same expected behavior.
    testAgainstDB(foreignDB);

    st.stop();
})();
