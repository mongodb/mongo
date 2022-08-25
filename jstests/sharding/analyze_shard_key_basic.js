/**
 * Tests support for the analyzeShardKey command.
 *
 * @tags: [requires_fcv_61, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

function runTestExistingNs(conn, ns, keyWithIndex, keyWithoutIndex) {
    jsTest.log(`Running analyzeShardKey command against an existing collection ${ns} on ${conn}`);

    assert.commandWorked(conn.adminCommand({analyzeShardKey: ns, key: keyWithIndex}));
    if (keyWithoutIndex) {
        assert.commandWorked(conn.adminCommand({analyzeShardKey: ns, key: keyWithoutIndex}));
    }

    // Cannot specify read/write concern.
    assert.commandFailedWithCode(
        conn.adminCommand(
            {analyzeShardKey: ns, key: keyWithIndex, readConcern: {level: "available"}}),
        ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(
        conn.adminCommand({analyzeShardKey: ns, key: keyWithIndex, writeConcern: {w: "majority"}}),
        ErrorCodes.InvalidOptions);
}

function runTestNonExistingNs(conn, ns, key) {
    jsTest.log(
        `Running analyzeShardKey command against a non-existing collection ${ns} on ${conn}`);
    assert.commandFailedWithCode(conn.adminCommand({analyzeShardKey: ns, key: key}),
                                 ErrorCodes.NamespaceNotFound);
}

{
    const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

    const dbName = "testDb";
    const nonExistingNs = dbName + ".nonExistingColl";
    const candidateKeyWithIndex = {candidateKey: 1};
    const candidateKeyWithoutIndex = {candidateKey: "hashed"};  // does not have a supporting index.

    const shardedNs = dbName + ".shardedColl";
    const currentKey = {currentKey: 1};
    const currentKeySplitPoint = {currentKey: 0};
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.name);
    assert.commandWorked(st.s.adminCommand({shardCollection: shardedNs, key: currentKey}));
    assert.commandWorked(st.s.adminCommand({split: shardedNs, middle: currentKeySplitPoint}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: shardedNs, find: currentKeySplitPoint, to: st.shard1.shardName}));
    assert.commandWorked(st.s.getCollection(shardedNs).createIndex(candidateKeyWithIndex));

    const unshardedNs = dbName + ".unshardedColl";
    assert.commandWorked(st.s.getCollection(unshardedNs).createIndex(candidateKeyWithIndex));

    // Verify that the command is supported on all shardsvr mongods (both primary and secondary).
    function runTestSupported(conn, isPrimaryShard) {
        // Can evaluate candidate shard keys for an existing unsharded collection.
        if (isPrimaryShard) {
            runTestExistingNs(conn, unshardedNs, candidateKeyWithIndex, candidateKeyWithoutIndex);
        } else {
            runTestNonExistingNs(conn, unshardedNs, candidateKeyWithIndex);
        }
        // Can evaluate the current shard key for an existing sharded collection.
        runTestExistingNs(conn, shardedNs, currentKey);
        // Can evaluate candidate shard keys for an existing sharded collection.
        runTestExistingNs(conn, shardedNs, candidateKeyWithIndex, candidateKeyWithoutIndex);
        // Cannot evaluate a candidate shard key for a non-existing collection.
        runTestNonExistingNs(conn, nonExistingNs, candidateKeyWithIndex);
    }

    st.rs0.nodes.forEach(node => {
        runTestSupported(node, true /* isPrimaryShard */);
    });
    st.rs1.nodes.forEach(node => {
        runTestSupported(node, false /* isPrimaryShard */);
    });

    // Verify that the command is not supported on configsvr mongods.
    function runTestNotSupported(conn) {
        assert.commandFailedWithCode(
            conn.adminCommand({analyzeShardKey: unshardedNs, key: candidateKeyWithIndex}),
            ErrorCodes.IllegalOperation);
        assert.commandFailedWithCode(
            conn.adminCommand({analyzeShardKey: shardedNs, key: currentKey}),
            ErrorCodes.IllegalOperation);
        assert.commandFailedWithCode(
            conn.adminCommand({analyzeShardKey: shardedNs, key: candidateKeyWithIndex}),
            ErrorCodes.IllegalOperation);
    }

    st.configRS.nodes.forEach(node => {
        runTestNotSupported(node);
    });

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    const dbName = "testDb";
    const nonExistingNs = dbName + ".nonExistingColl";
    const candidateKey = {candidateKey: 1};
    const invalidCandidateKey = {candidateKey: "hashed"};  // does not have a supporting index.

    const unshardedNs = dbName + ".unshardedColl";
    assert.commandWorked(primary.getCollection(unshardedNs).createIndex(candidateKey));

    // Verify that the command is supported on all mongods (both primary and secondary).
    function runTestSupported(conn) {
        // Can evaluate a candidate shard key for an existing unsharded collection.
        runTestExistingNs(conn, unshardedNs, candidateKey, invalidCandidateKey);
        // Cannot evaluate a candidate shard key for a non-existing collection.
        runTestNonExistingNs(conn, nonExistingNs, candidateKey);
    }
    rst.nodes.forEach(node => {
        runTestSupported(node);
    });

    rst.stopSet();
}
})();
