// Tests for $merge with a non-existing target collection.
(function() {
"use strict";

load("jstests/aggregation/extras/merge_helpers.js");  // For withEachMergeMode.
load("jstests/aggregation/extras/utils.js");          // For assertErrorCode.

const st = new ShardingTest({shards: 2, rs: {nodes: 1}, config: 1});
const sourceDB = st.s0.getDB("source_db");

/**
 * Run an aggregation on 'sourceColl' that writes documents to 'targetColl' with $merge.
 */
function testMerge(sourceColl, targetColl, shardedSource) {
    sourceColl.drop();

    if (shardedSource) {
        st.shardColl(sourceColl, {_id: 1}, {_id: 0}, {_id: 1}, sourceDB.getName());
    }

    for (let i = 0; i < 10; i++) {
        assert.commandWorked(sourceColl.insert({_id: i}));
    }

    // Test the behavior for each of the $merge modes. Since the target collection does not
    // exist, the behavior should be identical.
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        // Skip the combination of merge modes which will fail depending on the contents of the
        // source and target collection, as this will cause the assertion below to trip.
        if (whenMatchedMode == "fail" || whenNotMatchedMode == "fail")
            return;

        targetColl.drop();
        sourceColl.aggregate([{
            $merge: {
                into: {db: targetColl.getDB().getName(), coll: targetColl.getName()},
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode,
                on: "_id"
            }
        }]);
        assert.eq(whenNotMatchedMode == "discard" ? 0 : 10, targetColl.find().itcount());
    });

    // Test that $merge fails if the "on" field is anything but "_id" when the target collection
    // does not exist.
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        // Skip the combination of merge modes which will fail depending on the contents of the
        // source and target collection, as this will cause the assertion below to trip.
        if (whenMatchedMode == "fail" || whenNotMatchedMode == "fail")
            return;

        targetColl.drop();
        assertErrorCode(
            sourceColl,
            [{
                $merge: {
                    into: {db: targetColl.getDB().getName(), coll: targetColl.getName()},
                    whenMatched: whenMatchedMode,
                    whenNotMatched: whenNotMatchedMode,
                    on: "not_allowed"
                }
            }],
            51190);
    });

    // If 'targetColl' is in the same database as 'sourceColl', test that the legacy $out works
    // correctly.
    if (targetColl.getDB() == sourceColl.getDB()) {
        jsTestLog(`Testing $out from ${sourceColl.getFullName()} ` +
                  `(${shardedSource ? "sharded" : "unsharded"}) to ${targetColl.getFullName()} ` +
                  `with legacy syntax`);

        targetColl.drop();
        sourceColl.aggregate([{$out: targetColl.getName()}]);
        assert.eq(10, targetColl.find().itcount());
    }
}

const sourceColl = sourceDB["source_coll"];
const outputCollSameDb = sourceDB["output_coll"];

// Test $merge from an unsharded source collection to a non-existent output collection in the
// same database.
testMerge(sourceColl, outputCollSameDb, false);

// Like the last test case, but perform a $merge from a sharded source collection to a
// non-existent output collection in the same database.
testMerge(sourceColl, outputCollSameDb, true);

// Test that $merge in a sharded cluster fails when the output is sent to a different database
// that doesn't exist.
const foreignDb = st.s0.getDB("foreign_db");
const outputCollDiffDb = foreignDb["output_coll"];
foreignDb.dropDatabase();
assert.throws(() => testMerge(sourceColl, outputCollDiffDb, false));
assert.throws(() => testMerge(sourceColl, outputCollDiffDb, true));

// Test $merge from an unsharded source collection to an output collection in a different
// database where the database exists but the collection does not.
assert.commandWorked(foreignDb["test"].insert({_id: "forcing database creation"}));
testMerge(sourceColl, outputCollDiffDb, false);

// Like the last test, but with a sharded source collection.
testMerge(sourceColl, outputCollDiffDb, true);
st.stop();
}());
