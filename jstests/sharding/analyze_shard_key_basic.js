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
    const shardedNs = dbName + ".shardedColl";
    const unshardedNs = dbName + ".unshardedColl";
    const nonExistingNs = dbName + ".nonExistingColl";
    const candidateKey0 = {candidateKey0: 1};
    const candidateKey1 = {candidateKey1: 1};  // does not have a supporting index.

    // Set up the sharded collection.
    const currentKey = {currentKey: 1};
    const currentKeySplitPoint = {currentKey: 0};
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.name);
    assert.commandWorked(st.s.adminCommand({shardCollection: shardedNs, key: currentKey}));
    assert.commandWorked(st.s.adminCommand({split: shardedNs, middle: currentKeySplitPoint}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: shardedNs, find: currentKeySplitPoint, to: st.shard1.shardName}));
    assert.commandWorked(st.s.getCollection(shardedNs).createIndex(candidateKey0));

    // Set up the unsharded collection.
    assert.commandWorked(st.s.getCollection(unshardedNs).createIndex(candidateKey0));

    // Verify that the command is supported on mongos and all shardsvr mongods (both primary and
    // secondary).
    function runTestSupported(conn, isPrimaryShardOrMongos) {
        // Can evaluate a candidate shard key for an unsharded collection.
        if (isPrimaryShardOrMongos) {
            runTestExistingNs(conn, unshardedNs, candidateKey0, candidateKey1);
        } else {
            runTestNonExistingNs(conn, unshardedNs, candidateKey0);
        }
        // Can evaluate the current shard key for an existing sharded collection.
        runTestExistingNs(conn, shardedNs, currentKey);
        // Can evaluate candidate shard keys for an existing sharded collection.
        runTestExistingNs(conn, shardedNs, candidateKey0, candidateKey1);
        // Cannot evaluate a candidate shard key for a non-existing collection.
        runTestNonExistingNs(conn, nonExistingNs, candidateKey0);
    }

    runTestSupported(st.s, true /* isPrimaryShardOrMongos */);
    st.rs0.nodes.forEach(node => {
        runTestSupported(node, true /* isPrimaryShardOrMongos */);
    });
    st.rs1.nodes.forEach(node => {
        runTestSupported(node, false /* isPrimaryShardOrMongos */);
    });

    // Verify that the command is not supported on configsvr mongods.
    function runTestNotSupported(conn) {
        assert.commandFailedWithCode(
            conn.adminCommand({analyzeShardKey: unshardedNs, key: candidateKey0}),
            ErrorCodes.IllegalOperation);
        assert.commandFailedWithCode(
            conn.adminCommand({analyzeShardKey: shardedNs, key: currentKey}),
            ErrorCodes.IllegalOperation);
        assert.commandFailedWithCode(
            conn.adminCommand({analyzeShardKey: shardedNs, key: candidateKey0}),
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
    const unshardedNs = dbName + ".unshardedColl";
    const nonExistingNs = dbName + ".nonExistingColl";
    const candidateKey0 = {candidateKey0: 1};
    const candidateKey1 = {candidateKey1: 1};  // does not have a supporting index.

    // Set up the unsharded collection.
    assert.commandWorked(primary.getCollection(unshardedNs).createIndex(candidateKey0));

    // Verify that the command is supported on all mongods (both primary and secondary).
    function runTestSupported(conn) {
        // Can evaluate a candidate shard key for an existing unsharded collection.
        runTestExistingNs(conn, unshardedNs, candidateKey0, candidateKey1);
        // Cannot evaluate a candidate shard key for a non-existing collection.
        runTestNonExistingNs(conn, nonExistingNs, candidateKey0);
    }
    rst.nodes.forEach(node => {
        runTestSupported(node);
    });

    rst.stopSet();
}
})();
