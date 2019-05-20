// Tests that the "on" fields are correctly automatically generated when the user does not specify
// it in the $merge stage.
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");               // For 'getAggPlanStage'.
    load("jstests/aggregation/extras/out_helpers.js");  // For withEachMergeMode.

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}, config: 1});

    const mongosDB = st.s0.getDB("merge_on_fields");
    const firstColl = mongosDB.first;
    const secondColl = mongosDB.second;
    const sourceCollection = mongosDB.source;
    assert.commandWorked(sourceCollection.insert([{a: 1, b: 1, c: 1, d: 1}, {a: 2, b: 2, c: 2}]));

    // Test that the unique key will be defaulted to the document key for a sharded collection.
    st.shardColl(firstColl.getName(),
                 {a: 1, b: 1, c: 1},
                 {a: 1, b: 1, c: 1},
                 {a: 1, b: MinKey, c: MinKey},
                 mongosDB.getName());

    // Write a document to each chunk.
    assert.commandWorked(firstColl.insert({_id: 1, a: -3, b: -5, c: -6}));
    assert.commandWorked(firstColl.insert({_id: 2, a: 5, b: 3, c: 2}));

    // Testing operations on the same sharded collection.
    let explainResult = sourceCollection.explain().aggregate(
        [{$merge: {into: firstColl.getName(), whenMatched: "fail", whenNotMatched: "insert"}}]);
    assert.setEq(new Set(["_id", "a", "b", "c"]),
                 new Set(getAggPlanStage(explainResult, "$merge").$merge.on));

    explainResult = sourceCollection.explain().aggregate([{
        $merge:
            {into: firstColl.getName(), whenMatched: "replaceWithNew", whenNotMatched: "insert"}
    }]);
    assert.setEq(new Set(["_id", "a", "b", "c"]),
                 new Set(getAggPlanStage(explainResult, "$merge").$merge.on));

    // Test it with a different collection and shard key pattern.
    st.shardColl(
        secondColl.getName(), {a: 1, b: 1}, {a: 1, b: 1}, {a: 1, b: MinKey}, mongosDB.getName());

    // Write a document to each chunk.
    assert.commandWorked(secondColl.insert({_id: 3, a: -1, b: -3, c: 5}));
    assert.commandWorked(secondColl.insert({_id: 4, a: 4, b: 5, c: 6}));

    explainResult = sourceCollection.explain().aggregate(
        [{$merge: {into: secondColl.getName(), whenMatched: "fail", whenNotMatched: "insert"}}]);
    assert.setEq(new Set(["_id", "a", "b"]),
                 new Set(getAggPlanStage(explainResult, "$merge").$merge.on));

    explainResult = sourceCollection.explain().aggregate([{
        $merge:
            {into: firstColl.getName(), whenMatched: "replaceWithNew", whenNotMatched: "insert"}
    }]);
    assert.setEq(new Set(["_id", "a", "b", "c"]),
                 new Set(getAggPlanStage(explainResult, "$merge").$merge.on));

    // Test that the "on" field is defaulted to _id for a collection which does not exist.
    const doesNotExist = mongosDB.doesNotExist;
    doesNotExist.drop();
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        explainResult = sourceCollection.explain().aggregate([{
            $merge: {
                into: doesNotExist.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        }]);
        assert.eq(["_id"], getAggPlanStage(explainResult, "$merge").$merge.on);
    });

    // Test that the "on" field is defaulted to _id for an unsharded collection.
    const unsharded = mongosDB.unsharded;
    unsharded.drop();
    assert.commandWorked(unsharded.insert({x: 1}));
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        explainResult = sourceCollection.explain().aggregate([{
            $merge: {
                into: unsharded.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        }]);
        assert.eq(["_id"], getAggPlanStage(explainResult, "$merge").$merge.on);
    });

    st.stop();
})();
