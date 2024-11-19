/**
 * Common test code for sharding/analyze_shard_key/analyze_shard_key_basic.js and
 * core_sharding/analyze_shard_key/analyze_shard_key.js
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";
import {getNonPrimaryShardName} from "jstests/sharding/libs/sharding_util.js";

export const setParameterOpts = {
    analyzeShardKeyNumRanges: 100
};

// The sampling-based initial split policy needs 10 samples per split point so
// 10 * analyzeShardKeyNumRanges is the minimum number of distinct shard key values that the
// collection must have for the command to not fail to generate split points.
const numDocs = 10 * setParameterOpts.analyzeShardKeyNumRanges;

export function testNonExistingCollection(dbName, testCases) {
    const collName = "testCollNonExisting";
    const ns = dbName + "." + collName;
    const candidateKey = {candidateKey: 1};

    testCases.forEach(testCase => {
        jsTest.log(`Running analyzeShardKey command against a non-existing collection: ${
            tojson(testCase)}`);
        const cmdObj = {analyzeShardKey: ns, key: candidateKey};
        const res = testCase.conn.adminCommand(cmdObj);
        // If the command is not supported, it should fail even before the collection validation
        // step. That is, it should fail with an IllegalOperation error instead of a
        // NamespaceNotFound error.
        const expectedErrorCode =
            testCase.isSupported ? ErrorCodes.NamespaceNotFound : ErrorCodes.IllegalOperation;
        assert.commandFailedWithCode(res, expectedErrorCode);
    });
}

export function testExistingUnshardedCollection(dbName, writeConn, testCases) {
    const collName = "testCollUnsharded";
    const ns = dbName + "." + collName;
    const db = writeConn.getDB(dbName);
    const coll = db.getCollection(collName);

    const candidateKey0 = {candidateKey0: 1};
    const candidateKey1 = {candidateKey1: 1};  // does not have a supporting index.
    assert.commandWorked(coll.createIndex(candidateKey0));
    if (!FixtureHelpers.isStandalone(db)) {
        FixtureHelpers.awaitReplication(db);
    }

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
    if (!FixtureHelpers.isStandalone(db)) {
        FixtureHelpers.awaitReplication(db);
    }

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

export function testExistingShardedCollection(dbName, mongos, testCases) {
    const collName = "testCollSharded";
    const ns = dbName + "." + collName;
    const db = mongos.getDB(dbName);
    const coll = db.getCollection(collName);
    const nonPrimaryShard = getNonPrimaryShardName(db);

    const currentKey = {currentKey: 1};
    const currentKeySplitPoint = {currentKey: 0};
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: currentKey}));
    assert.commandWorked(mongos.adminCommand({split: ns, middle: currentKeySplitPoint}));
    assert.commandWorked(
        mongos.adminCommand({moveChunk: ns, find: currentKeySplitPoint, to: nonPrimaryShard}));

    const candidateKey0 = {candidateKey0: 1};
    const candidateKey1 = {candidateKey1: 1};  // does not have a supporting index
    assert.commandWorked(coll.createIndex(candidateKey0));
    FixtureHelpers.awaitReplication(db);

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
    FixtureHelpers.awaitReplication(db);

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
