// Tests for $merge with an existing target collection.
(function() {
"use strict";

load("jstests/aggregation/extras/merge_helpers.js");  // For withEachMergeMode.
load("jstests/aggregation/extras/utils.js");          // For assertErrorCode.

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
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        // Test without documents in target collection.
        assert.commandWorked(targetColl.remove({}));
        if (whenNotMatchedMode == "fail") {
            // Test whenNotMatchedMode: "fail" to an existing collection.
            assertErrorCode(sourceColl,
                            [{
                                $merge: {
                                    into: {
                                        db: targetColl.getDB().getName(),
                                        coll: targetColl.getName(),
                                    },
                                    whenMatched: whenMatchedMode,
                                    whenNotMatched: whenNotMatchedMode
                                }
                            }],
                            13113);
        } else {
            assert.doesNotThrow(() => sourceColl.aggregate([{
                $merge: {
                    into: {
                        db: targetColl.getDB().getName(),
                        coll: targetColl.getName(),
                    },
                    whenMatched: whenMatchedMode,
                    whenNotMatched: whenNotMatchedMode
                }
            }]));
            assert.eq(whenNotMatchedMode == "discard" ? 0 : 10, targetColl.find().itcount());
        }

        // Test with documents in target collection. Every document in the source collection is
        // present in the target, plus some additional documents that doesn't match.
        assert.commandWorked(targetColl.remove({}));
        for (let i = -10; i < 5; i++) {
            assert.commandWorked(targetColl.insert({_id: i}));
        }

        if (whenMatchedMode == "fail") {
            // Test whenMatched: "fail" to an existing collection with unique key conflicts.
            assertErrorCode(sourceColl,
                            [{
                                $merge: {
                                    into: {
                                        db: targetColl.getDB().getName(),
                                        coll: targetColl.getName(),
                                    },
                                    whenMatched: whenMatchedMode,
                                    whenNotMatched: whenNotMatchedMode
                                }
                            }],
                            ErrorCodes.DuplicateKey);
        } else {
            assert.doesNotThrow(() => sourceColl.aggregate([{
                $merge: {
                    into: {
                        db: targetColl.getDB().getName(),
                        coll: targetColl.getName(),
                    },
                    whenMatched: whenMatchedMode,
                    whenNotMatched: whenNotMatchedMode
                }
            }]));
        }
        assert.eq(15, targetColl.find().itcount());
    });

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
