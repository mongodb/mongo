/**
 * Tests support for the analyzeShardKey command.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey, temporary_catalog_shard_incompatible]
 */
(function() {
"use strict";

load("jstests/libs/catalog_shard_util.js");

const setParameterOpts = {
    analyzeShardKeyNumRanges: 100
};
const dbNameBase = "testDb";
// The sampling-based initial split policy needs 10 samples per split point so
// 10 * analyzeShardKeyNumRanges is the minimum number of distinct shard key values that the
// collection must have for the command to not fail to generate split points.
const numDocs = 10 * setParameterOpts.analyzeShardKeyNumRanges;

function testNonExistingCollection(testCases, tenantId) {
    const dbName = tenantId ? (tenantId + "-" + dbNameBase) : dbNameBase;
    const collName = "testCollNonExisting";
    const ns = dbName + "." + collName;
    const candidateKey = {candidateKey: 1};

    testCases.forEach(testCase => {
        jsTest.log(`Running analyzeShardKey command against a non-existing collection: ${
            tojson(testCase)}`);
        const cmdObj = {analyzeShardKey: ns, key: candidateKey};
        if (tenantId) {
            cmdObj.$tenant = tenantId;
        }
        const res = testCase.conn.adminCommand(cmdObj);
        // If the command is not supported, it should fail even before the collection validation
        // step. That is, it should fail with an IllegalOperation error instead of a
        // NamespaceNotFound error.
        const expectedErrorCode =
            testCase.isSupported ? ErrorCodes.NamespaceNotFound : ErrorCodes.IllegalOperation;
        assert.commandFailedWithCode(res, expectedErrorCode);
    });
}

function testExistingUnshardedCollection(writeConn, testCases) {
    const dbName = dbNameBase;
    const collName = "testCollUnsharded";
    const ns = dbName + "." + collName;
    const coll = writeConn.getCollection(ns);

    const candidateKey0 = {candidateKey0: 1};
    const candidateKey1 = {candidateKey1: 1};  // does not have a supporting index.
    assert.commandWorked(coll.createIndex(candidateKey0));

    // Analyze shard keys while the collection is empty.
    testCases.forEach(testCase => {
        jsTest.log(`Running analyzeShardKey command against an empty unsharded collection: ${
            tojson(testCase)}`);

        const res0 = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey0});
        const res1 = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey1});
        if (testCase.isSupported) {
            // This is an unsharded collection so in a sharded cluster it only exists on the
            // primary shard.
            let expectedErrCode = (() => {
                if (testCase.isMongos || testCase.isReplSetMongod) {
                    return ErrorCodes.IllegalOperation;
                } else if (testCase.isPrimaryShardMongod) {
                    return ErrorCodes.CollectionIsEmptyLocally;
                }
                return ErrorCodes.NamespaceNotFound;
            })();
            assert.commandFailedWithCode(res0, expectedErrCode);
            assert.commandFailedWithCode(res1, expectedErrCode);
        } else {
            assert.commandFailedWithCode(res0, ErrorCodes.IllegalOperation);
            assert.commandFailedWithCode(res1, ErrorCodes.IllegalOperation);
        }
    });

    // Analyze shard keys while the collection is non-empty.
    const docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({candidateKey0: i, candidateKey1: i});
    }
    assert.commandWorked(coll.insert(docs));
    testCases.forEach(testCase => {
        jsTest.log(`Running analyzeShardKey command against a non-empty unsharded collection: ${
            tojson(testCase)}`);

        const res0 = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey0});
        const res1 = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey1});
        if (testCase.isSupported) {
            // This is an unsharded collection so in a sharded cluster it only exists on the
            // primary shard.
            if (testCase.isReplSetMongod || testCase.isPrimaryShardMongod || testCase.isMongos) {
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
    const dbName = dbNameBase;
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

    // Analyze shard keys while the collection is empty.
    testCases.forEach(testCase => {
        jsTest.log(`Running analyzeShardKey command against an empty sharded collection: ${
            tojson(testCase)}`);

        const res = testCase.conn.adminCommand({analyzeShardKey: ns, key: currentKey});
        const res0 = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey0});
        const res1 = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey1});
        if (testCase.isSupported) {
            const expectedErrCode = (() => {
                if (testCase.isMongos) {
                    return ErrorCodes.IllegalOperation;
                }
                if (testCase.doNotExpectColl) {
                    return ErrorCodes.NamespaceNotFound;
                }
                return ErrorCodes.CollectionIsEmptyLocally;
            })();
            assert.commandFailedWithCode(res, expectedErrCode);
            assert.commandFailedWithCode(res0, expectedErrCode);
            assert.commandFailedWithCode(res1, expectedErrCode);
        } else {
            assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
            assert.commandFailedWithCode(res0, ErrorCodes.IllegalOperation);
            assert.commandFailedWithCode(res1, ErrorCodes.IllegalOperation);
        }
    });

    // Analyze shard keys while the collection is non-empty.
    const docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({currentKey: -i, candidateKey0: -i, candidateKey1: -i});
    }
    assert.commandWorked(coll.insert(docs));
    testCases.forEach(testCase => {
        jsTest.log(`Running analyzeShardKey command against a non-empty sharded collection: ${
            tojson(testCase)}`);

        const res = testCase.conn.adminCommand({analyzeShardKey: ns, key: currentKey});
        const res0 = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey0});
        const res1 = testCase.conn.adminCommand({analyzeShardKey: ns, key: candidateKey1});
        if (testCase.isSupported) {
            if (testCase.doNotExpectColl) {
                assert.commandFailedWithCode(res, ErrorCodes.NamespaceNotFound);
                assert.commandFailedWithCode(res0, ErrorCodes.NamespaceNotFound);
                assert.commandFailedWithCode(res1, ErrorCodes.NamespaceNotFound);
            } else {
                assert.commandWorked(res);
                assert.commandWorked(res0);
                assert.commandWorked(res1);
            }
        } else {
            assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
            assert.commandFailedWithCode(res0, ErrorCodes.IllegalOperation);
            assert.commandFailedWithCode(res1, ErrorCodes.IllegalOperation);
        }
    });
}

function testNotSupportReadWriteConcern(writeConn, testCases) {
    const dbName = dbNameBase;
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
    const st = new ShardingTest({shards: 2, rs: {nodes: 2, setParameter: setParameterOpts}});

    assert.commandWorked(st.s.adminCommand({enableSharding: dbNameBase}));
    st.ensurePrimaryShard(dbNameBase, st.shard0.name);

    const testCases = [];
    // The analyzeShardKey command is supported on mongos and all shardsvr mongods (both primary and
    // secondary).
    testCases.push({conn: st.s, isSupported: true, isMongos: true});
    st.rs0.nodes.forEach(node => {
        testCases.push({conn: node, isSupported: true, isPrimaryShardMongod: true});
    });
    st.rs1.nodes.forEach(node => {
        testCases.push({conn: node, isSupported: true, isPrimaryShardMongod: false});
    });

    // The analyzeShardKey command is not supported on dedicated configsvr mongods.
    const isCatalogShardEnabled = CatalogShardUtil.isEnabledIgnoringFCV(st);
    st.configRS.nodes.forEach(node => {
        // Don't expect a sharded collection since the config server isn't enabled as a shard and
        // won't have chunks.
        testCases.push({conn: node, isSupported: isCatalogShardEnabled, doNotExpectColl: true});
    });

    testNonExistingCollection(testCases);
    testExistingUnshardedCollection(st.s, testCases);
    testExistingShardedCollection(st, testCases);
    testNotSupportReadWriteConcern(st.s, testCases);

    st.stop();
}

{
    const rst = new ReplSetTest({
        name: jsTest.name() + "_non_multitenant",
        nodes: 2,
        nodeOptions: {setParameter: setParameterOpts}
    });
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    const testCases = [];
    // The analyzeShardKey command is supported on all mongods (both primary and secondary).
    rst.nodes.forEach(node => {
        testCases.push({conn: node, isSupported: true, isReplSetMongod: true});
    });

    testExistingUnshardedCollection(primary, testCases);
    testNonExistingCollection(testCases);
    testNotSupportReadWriteConcern(primary, testCases);

    rst.stopSet();
}

if (!TestData.auth) {
    const rst = new ReplSetTest({
        name: jsTest.name() + "_multitenant",
        nodes: 1,
        nodeOptions: {
            auth: "",
            setParameter: Object.assign({}, setParameterOpts, {multitenancySupport: true})
        }
    });
    rst.startSet({keyFile: "jstests/libs/key1"});
    rst.initiate();
    const primary = rst.getPrimary();
    const adminDb = primary.getDB("admin");
    const tenantId = ObjectId();

    // Prepare a user for testing multitenancy via $tenant.
    // Must be authenticated as a user with ActionType::useTenant in order to use $tenant.
    assert.commandWorked(
        adminDb.runCommand({createUser: "admin", pwd: "pwd", roles: ["__system"]}));
    assert(adminDb.auth("admin", "pwd"));

    // The analyzeShardKey command is not supported in multitenancy.
    const testCases = [{conn: adminDb, isSupported: false}];
    testNonExistingCollection(testCases, tenantId);
    rst.stopSet();
}

{
    const mongod = MongoRunner.runMongod();

    // The analyzeShardKey command is not supported on standalone mongod.
    const testCases = [{conn: mongod, isSupported: false}];
    testExistingUnshardedCollection(mongod, testCases);

    MongoRunner.stopMongod(mongod);
}
})();
