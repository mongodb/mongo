/**
 * Tests support for the analyzeShardKey command.
 *
 * @tags: [requires_fcv_62, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

const dbName = "testDb";

function testNonExistingCollection(testCases) {
    const collName = "testCollNonExisting";
    const ns = dbName + "." + collName;
    const candidateKey = {candidateKey: 1};

    testCases.forEach(testCase => {
        jsTest.log(`Running analyzeShardKey command against a non-existing collection: ${
            tojson(testCase)}`);
        const res = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey});
        const expectedErrorCode =
            testCase.isSupported ? ErrorCodes.NamespaceNotFound : ErrorCodes.IllegalOperation;
        assert.commandFailedWithCode(res, expectedErrorCode);
    });
}

function testExistingUnshardedCollection(writeConn, testCases) {
    const collName = "testCollUnsharded";
    const ns = dbName + "." + collName;
    const coll = writeConn.getCollection(ns);

    const candidateKey0 = {candidateKey0: 1};
    const candidateKey1 = {candidateKey1: 1};  // does not have a supporting index.
    assert.commandWorked(coll.createIndex(candidateKey0));

    // Analyze shard keys while the collection is non-empty.
    assert.commandWorked(coll.insert([{candidateKey0: 1, candidateKey1: 1}]));
    testCases.forEach(testCase => {
        jsTest.log(`Running analyzeShardKey command against a non-empty unsharded collection: ${
            tojson(testCase)}`);

        const res0 = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey0});
        const res1 = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey1});
        if (testCase.isSupported) {
            // This is an unsharded collection so in a sharded cluster it only exists on the
            // primary shard.
            if (testCase.isNonShardsvrMongod || testCase.isPrimaryShardMongod ||
                testCase.isMongos) {
                assert.commandWorked(res0);
                assert.commandWorked(res1);
            } else {
                assert.commandFailedWithCode(res0, ErrorCodes.NamespaceNotFound);
                assert.commandFailedWithCode(res1, ErrorCodes.NamespaceNotFound);
            }
        } else {
            assert.commandFailedWithCode(res0, ErrorCodes.IllegalOperation);
            assert.commandFailedWithCode(res1, ErrorCodes.IllegalOperation);
        }
    });
}

function testExistingShardedCollection(st, testCases) {
    const collName = "testCollSharded";
    const ns = dbName + "." + collName;
    const coll = st.s.getCollection(ns);
    const primaryShard = st.getPrimaryShard(dbName);
    const nonPrimaryShard = st.getOther(primaryShard);

    const currentKey = {currentKey: 1};
    const currentKeySplitPoint = {currentKey: 0};
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: currentKey}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: currentKeySplitPoint}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: ns, find: currentKeySplitPoint, to: nonPrimaryShard.shardName}));

    const candidateKey0 = {candidateKey0: 1};
    const candidateKey1 = {candidateKey1: 1};  // does not have a supporting index
    assert.commandWorked(coll.createIndex(candidateKey0));

    // Analyze shard keys while the collection is non-empty.
    assert.commandWorked(coll.insert([
        {currentKey: -1, candidateKey0: -1, candidateKey1: -1},
        {currentKey: 1, candidateKey0: 1, candidateKey1: 1}
    ]));
    testCases.forEach(testCase => {
        jsTest.log(`Running analyzeShardKey command against a non-empty sharded collection: ${
            tojson(testCase)}`);

        const res = testCase.conn.adminCommand({analyzeShardKey: ns, key: currentKey});
        const res0 = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey0});
        const res1 = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey1});
        if (testCase.isSupported) {
            assert.commandWorked(res);
            assert.commandWorked(res0);
            assert.commandWorked(res1);
        } else {
            assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
            assert.commandFailedWithCode(res0, ErrorCodes.IllegalOperation);
            assert.commandFailedWithCode(res1, ErrorCodes.IllegalOperation);
        }
    });
}

function testNotSupportReadWriteConcern(writeConn, testCases) {
    const collName = "testCollReadWriteConcern";
    const ns = dbName + "." + collName;
    const coll = writeConn.getCollection(ns);

    const candidateKey = {candidateKey: 1};
    assert.commandWorked(coll.createIndex(candidateKey));
    assert.commandWorked(coll.insert([{candidateKey: 0}]));

    testCases.forEach(testCase => {
        assert.commandFailedWithCode(
            testCase.conn.adminCommand(
                {analyzeShardKey: ns, key: candidateKey, readConcern: {level: "available"}}),
            ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(
            testCase.conn.adminCommand(
                {analyzeShardKey: ns, key: candidateKey, writeConcern: {w: "majority"}}),
            ErrorCodes.InvalidOptions);
    });
}

{
    const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.name);

    // The analyzeShardKey command is supported on mongos and all shardsvr mongods (both primary and
    // secondary).
    const testCases = [];
    testCases.push({conn: st.s, isSupported: true, isMongos: true});
    st.rs0.nodes.forEach(node => {
        testCases.push({conn: node, isSupported: true, isPrimaryShardMongod: true});
    });
    st.rs1.nodes.forEach(node => {
        testCases.push({conn: node, isSupported: true, isPrimaryShardMongod: false});
    });
    // The analyzeShardKey command is not supported on configsvr mongods.
    st.configRS.nodes.forEach(node => {
        testCases.push({conn: node, isSupported: false});
    });

    testNonExistingCollection(testCases);
    testExistingUnshardedCollection(st.s, testCases);
    testExistingShardedCollection(st, testCases);
    testNotSupportReadWriteConcern(st.s, testCases);

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    // The analyzeShardKey command is supported on all mongods (both primary and secondary).
    const testCases = [];
    rst.nodes.forEach(node => {
        testCases.push({conn: node, isSupported: true, isNonShardsvrMongod: true});
    });

    testExistingUnshardedCollection(primary, testCases);
    testNonExistingCollection(testCases);
    testNotSupportReadWriteConcern(primary, testCases);

    rst.stopSet();
}
})();
