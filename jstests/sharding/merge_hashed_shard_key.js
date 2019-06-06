// Tests that the $merge stage works correctly when the shard key is hashed. This includes the case
// when the "on" field is not explicitly specified and also when there is a unique, non-hashed index
// that matches the "on" field(s).
(function() {
    "use strict";

    load("jstests/aggregation/extras/merge_helpers.js");  // For withEachMergeMode,
                                                          // assertMergeFailsWithoutUniqueIndex,
    // assertMergeSucceedsWithExpectedUniqueIndex.

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}, config: 1});

    const mongosDB = st.s0.getDB("merge_hashed_shard_key");
    const foreignDB = st.s0.getDB("merge_hashed_shard_key_foreign");
    const source = mongosDB.source;
    const target = mongosDB.target;
    source.drop();
    target.drop();

    assert.commandWorked(source.insert({placeholderDoc: 1}));

    function testHashedShardKey(shardKey, spec, prefixPipeline = []) {
        target.drop();
        st.shardColl(target, shardKey, spec);

        // Test that $merge passes without specifying an "on" field.
        assertMergeSucceedsWithExpectedUniqueIndex(
            {source: source, target: target, prevStages: prefixPipeline});

        // Test that $merge fails even if the "on" fields matches the shardKey, since it isn't
        // unique.
        assertMergeFailsWithoutUniqueIndex({
            source: source,
            target: target,
            onFields: Object.keys(shardKey),
            prevStages: prefixPipeline
        });

        // Test that the $merge passes if there exists a unique index prefixed on the hashed shard
        // key.
        const prefixedUniqueKey = Object.merge(shardKey, {extraField: 1});
        prefixPipeline = prefixPipeline.concat([{$addFields: {extraField: 1}}]);
        assert.commandWorked(target.createIndex(prefixedUniqueKey, {unique: true}));
        assertMergeSucceedsWithExpectedUniqueIndex(
            {source: source, target: target, prevStages: prefixPipeline});
        assertMergeSucceedsWithExpectedUniqueIndex({
            source: source,
            target: target,
            onFields: Object.keys(prefixedUniqueKey),
            prevStages: prefixPipeline
        });
    }

    //
    // Tests for a hashed non-id shard key.
    //
    let prevStage = [{$addFields: {hashedKey: 1}}];
    testHashedShardKey({hashedKey: 1}, {hashedKey: "hashed"}, prevStage);

    //
    // Tests for a hashed non-id dotted path shard key.
    //
    prevStage = [{$addFields: {dotted: {path: 1}}}];
    testHashedShardKey({"dotted.path": 1}, {"dotted.path": "hashed"}, prevStage);

    //
    // Tests for a compound hashed shard key.
    //
    prevStage = [{$addFields: {hashedKey: {subField: 1}, nonHashedKey: 1}}];
    testHashedShardKey({"hashedKey.subField": 1, nonHashedKey: 1},
                       {"hashedKey.subField": "hashed", nonHashedKey: 1},
                       prevStage);

    //
    // Tests for a hashed _id shard key.
    //
    target.drop();
    st.shardColl(target, {_id: 1}, {_id: "hashed"});

    // Test that $merge passes without specifying an "on" field.
    assertMergeSucceedsWithExpectedUniqueIndex({source: source, target: target});

    // Test that $merge passes when the uniqueKey matches the shard key. Note that the _id index is
    // always create with {unique: true} regardless of whether the shard key was marked as unique
    // when the collection was sharded.
    assertMergeSucceedsWithExpectedUniqueIndex(
        {source: source, target: target, uniqueKey: {_id: 1}});

    st.stop();
})();
