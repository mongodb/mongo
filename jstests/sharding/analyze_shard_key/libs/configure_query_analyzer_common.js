/**
 * Defines common functions for testing the configureQueryAnalyzer command.
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {QuerySamplingUtil} from "jstests/sharding/analyze_shard_key/libs/query_sampling_util.js";
import {
    moveDatabaseAndUnshardedColls
} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";
import {getPrimaryShardIdForDatabase} from "jstests/sharding/libs/sharding_util.js";

/**
 * Tests that the configureQueryAnalyzer command does not work for a non-existing collection.
 */
export function testNonExistingCollection(testCases, dbName) {
    const collName = "testCollNonExisting";
    const ns = dbName + "." + collName;

    testCases.forEach(testCase => {
        jsTest.log(`Running configureQueryAnalyzer command against an non-existing collection: ${
            tojson({testCase, ns})}`);
        const cmdObj = {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 1};
        const res = testCase.conn.adminCommand(cmdObj);
        const expectedErrorCode =
            testCase.expectedErrorCode ? testCase.expectedErrorCode : ErrorCodes.NamespaceNotFound;
        assert.commandFailedWithCode(res, expectedErrorCode);
    });
}

/**
 * Tests the configureQueryAnalyzer command with with various options for an existing collection.
 */
export function testExistingCollection(writeConn, testCases, dbName) {
    const collName = "testCollUnsharded";
    const ns = dbName + "." + collName;
    const db = writeConn.getDB(dbName);
    assert.commandWorked(db.createCollection(collName));

    testCases.forEach(testCase => {
        if (testCase.conn.isConfigsvr) {
            // The collection created below will not exist on the config server.
            return;
        }

        jsTest.log(
            `Running configureQueryAnalyzer command against an existing collection:
         ${tojson(testCase)}`);

        // Can set 'samplesPerSecond' to > 0.
        const basicRes = testCase.conn.adminCommand(
            {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 0.1});
        if (testCase.expectedErrorCode) {
            assert.commandFailedWithCode(basicRes, testCase.expectedErrorCode);
            // There is no need to test the remaining cases.
            return;
        }
        assert.commandWorked(basicRes);
        assert.commandWorked(testCase.conn.adminCommand(
            {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 1}));
        assert.commandWorked(testCase.conn.adminCommand(
            {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 50}));

        // Cannot set 'samplesPerSecond' to 0.
        assert.commandFailedWithCode(
            testCase.conn.adminCommand(
                {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 0}),
            ErrorCodes.InvalidOptions);

        // This check only applies when testingDiagnosticsEnabled is false. However for core tests,
        // this parameter is always true and cannot be modified (the parameter is not runtime
        // modifiable and core test server fixtures cannot be restarted).
        if (!testCase.isCoreTest) {
            // Cannot set 'samplesPerSecond' to larger than 50.
            assert.commandFailedWithCode(
                testCase.conn.adminCommand(
                    {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 51}),
                ErrorCodes.InvalidOptions);
        }

        // Cannot specify 'samplesPerSecond' when 'mode' is "off".
        assert.commandFailedWithCode(
            testCase.conn.adminCommand(
                {configureQueryAnalyzer: ns, mode: "off", samplesPerSecond: 1}),
            ErrorCodes.InvalidOptions);
        assert.commandWorked(testCase.conn.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));

        // Cannot specify read/write concern.
        assert.commandFailedWithCode(testCase.conn.adminCommand({
            configureQueryAnalyzer: ns,
            mode: "full",
            samplesPerSecond: 1,
            readConcern: {level: "available"}
        }),
                                     ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(testCase.conn.adminCommand({
            configureQueryAnalyzer: ns,
            mode: "full",
            samplesPerSecond: 1,
            writeConcern: {w: "majority"}
        }),
                                     ErrorCodes.InvalidOptions);
    });
}

/**
 * Asserts that the query analyzer config doc matches what is expected.
 */
function assertQueryAnalyzerConfigDoc(
    conn, ns, collUuid, mode, samplesPerSecond, startTime, stopTime) {
    const doc = conn.getCollection("config.queryAnalyzers").findOne({_id: ns});
    assert.eq(doc.collUuid, collUuid, doc);
    assert.eq(doc.mode, mode, doc);
    assert.eq(doc.samplesPerSecond, samplesPerSecond, doc);
    assert(doc.hasOwnProperty("startTime"), doc);
    if (startTime) {
        assert.eq(doc.startTime, startTime, doc);
    }
    assert.eq(doc.hasOwnProperty("stopTime"), mode == "off", doc);
    if (stopTime) {
        assert.eq(doc.stopTime, stopTime, doc);
    }
    return doc;
}

/**
 * Asserts that the query analyzer config doc does not exist.
 */
function assertNoQueryAnalyzerConfigDoc(conn, ns) {
    const doc = conn.getCollection("config.queryAnalyzers").findOne({_id: ns});
    assert.eq(doc, null, doc);
}

/**
 * Asserts that the new and old query analyzer configurations returned in a response match what is
 * expected.
 */
function assertConfigQueryAnalyzerResponse(res, newConfig, oldConfig) {
    assert.eq(res.newConfiguration, newConfig, res);
    if (oldConfig) {
        assert.eq(res.oldConfiguration, oldConfig, res);
    } else {
        assert(!res.hasOwnProperty("oldConfiguration"), res);
    }
}

/**
 * Sets up a collection used by the query analyzer config doc functions.
 */
function setUpCollection(conn, {isShardedColl, isShardedCluster, shardNames}) {
    const dbName = "configure-query-analyzer" + extractUUIDFromObject(UUID());
    if (isShardedCluster) {
        assert.commandWorked(
            conn.adminCommand({enableSharding: dbName, primaryShard: shardNames[0]}));
    }
    const collName = isShardedColl ? "testCollSharded" : "testCollUnsharded";
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);

    assert.commandWorked(db.createCollection(collName));
    if (isShardedColl) {
        assert.commandWorked(conn.adminCommand({shardCollection: ns, key: {x: 1}}));
        assert.commandWorked(conn.adminCommand({split: ns, middle: {x: 0}}));
        assert.commandWorked(conn.adminCommand({moveChunk: ns, find: {x: 0}, to: shardNames[1]}));
    }

    return {dbName, collName};
}

/**
 * Tests that the configureQueryAnalyzer command correctly persists the config doc.
 */
export function testPersistingConfiguration(conn) {
    const {dbName, collName} = setUpCollection(conn, {isShardedColl: false});
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);
    let collUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    jsTest.log(
        `Testing that the configureQueryAnalyzer command persists the configuration correctly ${
            tojson({dbName, collName, collUuid})}`);

    // Run a configureQueryAnalyzer command to disable query sampling. Verify that the command
    // does not fail although query sampling is not even active.
    const mode0 = "off";
    const res0 = assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode: mode0}));
    assertConfigQueryAnalyzerResponse(res0, {mode: mode0} /* newConfig */);
    assertNoQueryAnalyzerConfigDoc(conn, ns);

    // Run a configureQueryAnalyzer command to enable query sampling.
    const mode1 = "full";
    const samplesPerSecond1 = 50;
    const res1 = assert.commandWorked(conn.adminCommand(
        {configureQueryAnalyzer: ns, mode: mode1, samplesPerSecond: samplesPerSecond1}));
    const doc1 = assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode1, samplesPerSecond1);
    assertConfigQueryAnalyzerResponse(
        res1, {mode: mode1, samplesPerSecond: samplesPerSecond1} /* newConfig */);

    // Run a configureQueryAnalyzer command to modify the sample rate. Verify that the 'startTime'
    // remains the same.
    const mode2 = "full";
    const samplesPerSecond2 = 0.2;
    const res2 = assert.commandWorked(conn.adminCommand(
        {configureQueryAnalyzer: ns, mode: mode2, samplesPerSecond: samplesPerSecond2}));
    assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode2, samplesPerSecond2, doc1.startTime);
    assertConfigQueryAnalyzerResponse(
        res2,
        {mode: mode2, samplesPerSecond: samplesPerSecond2} /* newConfig */,
        {mode: mode1, samplesPerSecond: samplesPerSecond1} /* oldConfig */);

    // Run a configureQueryAnalyzer command to disable query sampling.
    const mode3 = "off";
    const res3 = assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode: mode3}));
    assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode3, samplesPerSecond2, doc1.startTime);
    assertConfigQueryAnalyzerResponse(
        res3,
        {mode: mode3} /* newConfig */,
        {mode: mode2, samplesPerSecond: samplesPerSecond2} /* oldConfig */);

    // Run a configureQueryAnalyzer command to re-enable query sampling. Verify that the 'startTime'
    // is new.
    const mode4 = "full";
    const samplesPerSecond4 = 1;
    const res4 = assert.commandWorked(conn.adminCommand(
        {configureQueryAnalyzer: ns, mode: mode4, samplesPerSecond: samplesPerSecond4}));
    const doc4 = assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode4, samplesPerSecond4);
    assert.gt(doc4.startTime, doc1.startTime, doc4);
    assertConfigQueryAnalyzerResponse(
        res4,
        {mode: mode4, samplesPerSecond: samplesPerSecond4} /* newConfig */,
        {mode: mode3, samplesPerSecond: samplesPerSecond2} /* oldConfig */);

    // Retry the previous configureQueryAnalyzer command. Verify that the 'startTime' remains the
    // same.
    const res4Retry = assert.commandWorked(conn.adminCommand(
        {configureQueryAnalyzer: ns, mode: mode4, samplesPerSecond: samplesPerSecond4}));
    assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode4, samplesPerSecond4, doc4.startTime);
    assertConfigQueryAnalyzerResponse(
        res4Retry,
        {mode: mode4, samplesPerSecond: samplesPerSecond4} /* newConfig */,
        {mode: mode4, samplesPerSecond: samplesPerSecond4} /* oldConfig */);

    assert(db.getCollection(collName).drop());
    assert.commandWorked(db.createCollection(collName));
    collUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    // Run a configureQueryAnalyzer command to re-enable query sampling after dropping the
    // collection. Verify that the 'startTime' is new, and "oldConfiguration" is not returned.
    const mode5 = "full";
    const samplesPerSecond5 = 0.1;
    const res5 = assert.commandWorked(conn.adminCommand(
        {configureQueryAnalyzer: ns, mode: mode5, samplesPerSecond: samplesPerSecond5}));
    const doc5 = assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode5, samplesPerSecond5);
    assert.gt(doc5.startTime, doc4.startTime, doc5);
    assertConfigQueryAnalyzerResponse(
        res5, {mode: mode5, samplesPerSecond: samplesPerSecond5} /* newConfig */);

    // Run a configureQueryAnalyzer command to disable query sampling. Verify that the
    // 'samplesPerSecond' doesn't get unset.
    const mode6 = "off";
    const res6 = assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode: mode6}));
    const doc6 =
        assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode6, samplesPerSecond5, doc5.startTime);
    assertConfigQueryAnalyzerResponse(
        res6,
        {mode: mode6} /* newConfig */,
        {mode: mode5, samplesPerSecond: samplesPerSecond5} /* oldConfig */);

    // Retry the previous configureQueryAnalyzer command. Verify that the retry does not fail and
    // that the 'stopTime' remains the same.
    assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode: mode6}));
    assertQueryAnalyzerConfigDoc(
        conn, ns, collUuid, mode6, samplesPerSecond5, doc5.startTime, doc6.stopTime);
}

/**
 * Tests that the config document is deleted when the collection being analyzed is dropped.
 */
export function testConfigurationDeletionDropCollection(
    conn, {isShardedColl, isShardedCluster, shardNames}) {
    const {dbName, collName} = setUpCollection(conn, {isShardedColl, isShardedCluster, shardNames});
    const ns = dbName + "." + collName;
    const collUuid = QuerySamplingUtil.getCollectionUuid(conn.getDB(dbName), collName);
    jsTest.log(`Testing configuration deletion upon dropCollection ${
        tojson({dbName, collName, isShardedColl})}`);

    const mode = "full";
    const samplesPerSecond = 0.5;
    const res = assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode, samplesPerSecond}));
    assertConfigQueryAnalyzerResponse(res, {mode, samplesPerSecond});
    assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode, samplesPerSecond);

    assert(conn.getDB(dbName).getCollection(collName).drop());
    if (isShardedCluster) {
        assertNoQueryAnalyzerConfigDoc(conn, ns);
    } else {
        // TODO (SERVER-76443): Make sure that dropCollection on replica set delete the
        // config.queryAnalyzers doc for the collection being dropped.
        assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode, samplesPerSecond);
    }
}

/**
 * Tests that the config document is deleted when the database for the collection being analyzed is
 * dropped.
 */
export function testConfigurationDeletionDropDatabase(
    conn, {isShardedColl, isShardedCluster, shardNames}) {
    const {dbName, collName} = setUpCollection(conn, {isShardedColl, isShardedCluster, shardNames});
    const ns = dbName + "." + collName;
    const collUuid = QuerySamplingUtil.getCollectionUuid(conn.getDB(dbName), collName);
    jsTest.log(`Testing configuration deletion upon dropDatabase ${
        tojson({dbName, collName, isShardedColl})}`);

    const mode = "full";
    const samplesPerSecond = 0.5;
    const res = assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode, samplesPerSecond}));
    assertConfigQueryAnalyzerResponse(res, {mode, samplesPerSecond});
    assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode, samplesPerSecond);

    assert.commandWorked(conn.getDB(dbName).dropDatabase());
    if (isShardedCluster) {
        assertNoQueryAnalyzerConfigDoc(conn, ns);
    } else {
        // TODO (SERVER-76443): Make sure that dropDatabase on replica set delete the
        // config.queryAnalyzers docs for all collections in the database being dropped.
        assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode, samplesPerSecond);
    }
}

/**
 * Tests that the config document is deleted when the collection being analyzed is renamed.
 */
export function testConfigurationDeletionRenameCollection(
    conn, {isShardedColl, isShardedCluster, shardNames, sameDatabase}) {
    const {dbName, collName} =
        setUpCollection(conn, {isShardedColl, isShardedCluster, shardNames, sameDatabase});

    const srcDbName = dbName;
    const srcCollName = collName;
    const srcNs = srcDbName + "." + srcCollName;
    const srcDb = conn.getDB(srcDbName);
    const srcCollUuid = QuerySamplingUtil.getCollectionUuid(srcDb, srcCollName);

    const dstDbName = sameDatabase ? srcDbName : (srcDbName + "New");
    const dstCollName = sameDatabase ? (srcCollName + "New") : srcCollName;
    const dstNs = dstDbName + "." + dstCollName;
    const dstDb = conn.getDB(dstDbName);
    assert.commandWorked(dstDb.createCollection(dstCollName));
    if (!sameDatabase && isShardedCluster) {
        // On a sharded cluster, the src and dst collections must be on same shard.
        moveDatabaseAndUnshardedColls(conn.getDB(dstDbName),
                                      getPrimaryShardIdForDatabase(conn, srcDbName));
    }
    const dstCollUuid = QuerySamplingUtil.getCollectionUuid(dstDb, dstCollName);

    jsTest.log(`Testing configuration deletion upon renameCollection ${
        tojson({sameDatabase, srcDbName, srcCollName, dstDbName, dstCollName, isShardedColl})}`);

    const mode = "full";
    const samplesPerSecond = 0.5;

    const srcRes = assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: srcNs, mode, samplesPerSecond}));
    assertConfigQueryAnalyzerResponse(srcRes, {mode, samplesPerSecond});
    assertQueryAnalyzerConfigDoc(conn, srcNs, srcCollUuid, mode, samplesPerSecond);

    const dstRes = assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: dstNs, mode, samplesPerSecond}));
    assertConfigQueryAnalyzerResponse(dstRes, {mode, samplesPerSecond});
    assertQueryAnalyzerConfigDoc(conn, dstNs, dstCollUuid, mode, samplesPerSecond);

    assert.commandWorked(conn.adminCommand({renameCollection: srcNs, to: dstNs, dropTarget: true}));
    if (isShardedCluster) {
        assertNoQueryAnalyzerConfigDoc(conn, srcNs);
        assertNoQueryAnalyzerConfigDoc(conn, dstNs);
    } else {
        // TODO (SERVER-76443): Make sure that renameCollection on replica set delete the
        // config.queryAnalyzers doc for the collection being renamed.
        assertQueryAnalyzerConfigDoc(conn, srcNs, srcCollUuid, mode, samplesPerSecond);
        assertQueryAnalyzerConfigDoc(conn, dstNs, dstCollUuid, mode, samplesPerSecond);
    }
}
