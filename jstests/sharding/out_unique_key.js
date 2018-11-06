// Tests that the uniquekey is correctly automatically generated when the user does not specify it
// in the $out stage.
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");  // For 'getAggPlanStage'.

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}, config: 1});

    const mongosDB = st.s0.getDB("out_unique_key");
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
        [{$out: {to: firstColl.getName(), mode: "insertDocuments"}}]);
    assert.eq({_id: 1, a: 1, b: 1, c: 1}, getAggPlanStage(explainResult, "$out").$out.uniqueKey);

    explainResult = sourceCollection.explain().aggregate(
        [{$out: {to: firstColl.getName(), mode: "replaceDocuments"}}]);
    assert.eq({_id: 1, a: 1, b: 1, c: 1}, getAggPlanStage(explainResult, "$out").$out.uniqueKey);

    // Test that the "legacy" mode AKA "replaceCollection" will not succeed when outputting to a
    // sharded collection, even for explain.
    let error = assert.throws(() => sourceCollection.aggregate([{$out: firstColl.getName()}]));
    assert.eq(error.code, 28769);
    error =
        assert.throws(() => sourceCollection.explain().aggregate([{$out: firstColl.getName()}]));
    assert.eq(error.code, 28769);

    // Test it with a different collection and shard key pattern.
    st.shardColl(
        secondColl.getName(), {a: 1, b: 1}, {a: 1, b: 1}, {a: 1, b: MinKey}, mongosDB.getName());

    // Write a document to each chunk.
    assert.commandWorked(secondColl.insert({_id: 3, a: -1, b: -3, c: 5}));
    assert.commandWorked(secondColl.insert({_id: 4, a: 4, b: 5, c: 6}));

    explainResult = sourceCollection.explain().aggregate(
        [{$out: {to: secondColl.getName(), mode: "insertDocuments"}}]);
    assert.eq({_id: 1, a: 1, b: 1}, getAggPlanStage(explainResult, "$out").$out.uniqueKey);

    explainResult = sourceCollection.explain().aggregate(
        [{$out: {to: firstColl.getName(), mode: "replaceDocuments"}}]);
    assert.eq({_id: 1, a: 1, b: 1, c: 1}, getAggPlanStage(explainResult, "$out").$out.uniqueKey);

    function withEachMode(callback) {
        callback("replaceCollection");
        callback("replaceDocuments");
        callback("insertDocuments");
    }

    // Test that the uniqueKey is defaulted to _id for a collection which does not exist.
    const doesNotExist = mongosDB.doesNotExist;
    doesNotExist.drop();
    withEachMode((mode) => {
        explainResult = sourceCollection.explain().aggregate(
            [{$out: {to: doesNotExist.getName(), mode: mode}}]);
        assert.eq({_id: 1}, getAggPlanStage(explainResult, "$out").$out.uniqueKey);
    });

    // Test that the uniqueKey is defaulted to _id for an unsharded collection.
    const unsharded = mongosDB.unsharded;
    unsharded.drop();
    assert.commandWorked(unsharded.insert({x: 1}));
    withEachMode((mode) => {
        explainResult =
            sourceCollection.explain().aggregate([{$out: {to: unsharded.getName(), mode: mode}}]);
        assert.eq({_id: 1}, getAggPlanStage(explainResult, "$out").$out.uniqueKey);
    });

    st.stop();
})();
