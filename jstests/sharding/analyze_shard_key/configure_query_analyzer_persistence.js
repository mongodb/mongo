/**
 * Tests that the configureQueryAnalyzer command persists the configuration in a document
 * in config.queryAnalyzers and that the document is deleted when the associated collection
 * is dropped.
 *
 * @tags: [requires_fcv_70]
 */

(function() {
"use strict";

load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

function assertConfigQueryAnalyzerResponse(res, newConfig, oldConfig) {
    assert.eq(res.newConfiguration, newConfig, res);
    if (oldConfig) {
        assert.eq(res.oldConfiguration, oldConfig, res);
    } else {
        assert(!res.hasOwnProperty("oldConfiguration"), res);
    }
}

function assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode, sampleRate, startTime, stopTime) {
    const doc = conn.getCollection("config.queryAnalyzers").findOne({_id: ns});
    assert.eq(doc.collUuid, collUuid, doc);
    assert.eq(doc.mode, mode, doc);
    assert.eq(doc.sampleRate, sampleRate, doc);
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

function assertNoQueryAnalyzerConfigDoc(conn, ns) {
    const doc = conn.getCollection("config.queryAnalyzers").findOne({_id: ns});
    assert.eq(doc, null, doc);
}

function setUpCollection(conn, {isShardedColl, st}) {
    const dbName = "testDb-" + extractUUIDFromObject(UUID());
    const collName = isShardedColl ? "testCollSharded" : "testCollUnsharded";
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);

    assert.commandWorked(db.createCollection(collName));
    if (isShardedColl) {
        assert(st);
        assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
        st.ensurePrimaryShard(dbName, st.shard0.name);
        assert.commandWorked(st.s0.adminCommand({shardCollection: ns, key: {x: 1}}));
        assert.commandWorked(st.s0.adminCommand({split: ns, middle: {x: 0}}));
        assert.commandWorked(
            st.s0.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
    }

    return {dbName, collName};
}

function testPersistingConfiguration(conn) {
    const {dbName, collName} = setUpCollection(conn, {isShardedColl: false});
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);
    let collUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    jsTest.log(
        `Testing that the configureQueryAnalyzer command persists the configuration correctly ${
            tojson({dbName, collName, collUuid})}`);

    // Run a configureQueryAnalyzer command to disable query sampling. Verify that the command
    // fails since query sampling is not even active.
    const mode0 = "off";
    assert.commandFailedWithCode(conn.adminCommand({configureQueryAnalyzer: ns, mode: mode0}),
                                 ErrorCodes.IllegalOperation);
    assertNoQueryAnalyzerConfigDoc(conn, ns);

    // Run a configureQueryAnalyzer command to enable query sampling.
    const mode1 = "full";
    const sampleRate1 = 100;
    const res1 = assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: mode1, sampleRate: sampleRate1}));
    const doc1 = assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode1, sampleRate1);
    assertConfigQueryAnalyzerResponse(res1, {mode: mode1, sampleRate: sampleRate1} /* newConfig */);

    // Run a configureQueryAnalyzer command to modify the sample rate. Verify that the 'startTime'
    // remains the same.
    const mode2 = "full";
    const sampleRate2 = 0.2;
    const res2 = assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: mode2, sampleRate: sampleRate2}));
    assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode2, sampleRate2, doc1.startTime);
    assertConfigQueryAnalyzerResponse(res2,
                                      {mode: mode2, sampleRate: sampleRate2} /* newConfig */,
                                      {mode: mode1, sampleRate: sampleRate1} /* oldConfig */);

    // Run a configureQueryAnalyzer command to disable query sampling.
    const mode3 = "off";
    const res3 = assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode: mode3}));
    assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode3, sampleRate2, doc1.startTime);
    assertConfigQueryAnalyzerResponse(res3,
                                      {mode: mode3} /* newConfig */,
                                      {mode: mode2, sampleRate: sampleRate2} /* oldConfig */);

    // Run a configureQueryAnalyzer command to re-enable query sampling. Verify that the 'startTime'
    // is new.
    const mode4 = "full";
    const sampleRate4 = 1;
    const res4 = assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: mode4, sampleRate: sampleRate4}));
    const doc4 = assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode4, sampleRate4);
    assert.gt(doc4.startTime, doc1.startTime, doc4);
    assertConfigQueryAnalyzerResponse(res4,
                                      {mode: mode4, sampleRate: sampleRate4} /* newConfig */,
                                      {mode: mode3, sampleRate: sampleRate2} /* oldConfig */);

    // Retry the previous configureQueryAnalyzer command. Verify that the 'startTime' remains the
    // same.
    const res4Retry = assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: mode4, sampleRate: sampleRate4}));
    assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode4, sampleRate4, doc4.startTime);
    assertConfigQueryAnalyzerResponse(res4Retry,
                                      {mode: mode4, sampleRate: sampleRate4} /* newConfig */,
                                      {mode: mode4, sampleRate: sampleRate4} /* oldConfig */);

    assert(db.getCollection(collName).drop());
    assert.commandWorked(db.createCollection(collName));
    collUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    // Run a configureQueryAnalyzer command to re-enable query sampling after dropping the
    // collection. Verify that the 'startTime' is new, and "oldConfiguration" is not returned.
    const mode5 = "full";
    const sampleRate5 = 0.1;
    const res5 = assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: mode5, sampleRate: sampleRate5}));
    const doc5 = assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode5, sampleRate5);
    assert.gt(doc5.startTime, doc4.startTime, doc5);
    assertConfigQueryAnalyzerResponse(res5, {mode: mode5, sampleRate: sampleRate5} /* newConfig */);

    // Run a configureQueryAnalyzer command to disable query sampling. Verify that the 'sampleRate'
    // doesn't get unset.
    const mode6 = "off";
    const res6 = assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode: mode6}));
    const doc6 =
        assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode6, sampleRate5, doc5.startTime);
    assertConfigQueryAnalyzerResponse(res6,
                                      {mode: mode6} /* newConfig */,
                                      {mode: mode5, sampleRate: sampleRate5} /* oldConfig */);

    // Retry the previous configureQueryAnalyzer command. Verify that the 'stopTime' remains the
    // same.
    assert.commandFailedWithCode(conn.adminCommand({configureQueryAnalyzer: ns, mode: mode6}),
                                 ErrorCodes.IllegalOperation);
    assertQueryAnalyzerConfigDoc(
        conn, ns, collUuid, mode6, sampleRate5, doc5.startTime, doc6.stopTime);
}

function testConfigurationDeletionDropCollection(conn, {isShardedColl, rst, st}) {
    const {dbName, collName} = setUpCollection(conn, {isShardedColl, rst, st});
    const ns = dbName + "." + collName;
    const collUuid = QuerySamplingUtil.getCollectionUuid(conn.getDB(dbName), collName);
    jsTest.log(`Testing configuration deletion upon dropCollection ${
        tojson({dbName, collName, isShardedColl})}`);

    const mode = "full";
    const sampleRate = 0.5;
    const res =
        assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode, sampleRate}));
    assertConfigQueryAnalyzerResponse(res, {mode, sampleRate});
    assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode, sampleRate);

    assert(conn.getDB(dbName).getCollection(collName).drop());
    if (st) {
        assertNoQueryAnalyzerConfigDoc(conn, ns);
    } else {
        // TODO (SERVER-76443): Make sure that dropCollection on replica set delete the
        // config.queryAnalyzers doc for the collection being dropped.
        assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode, sampleRate);
    }
}

function testConfigurationDeletionDropDatabase(conn, {isShardedColl, rst, st}) {
    const {dbName, collName} = setUpCollection(conn, {isShardedColl, rst, st});
    const ns = dbName + "." + collName;
    const collUuid = QuerySamplingUtil.getCollectionUuid(conn.getDB(dbName), collName);
    jsTest.log(`Testing configuration deletion upon dropDatabase ${
        tojson({dbName, collName, isShardedColl})}`);

    const mode = "full";
    const sampleRate = 0.5;
    const res =
        assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode, sampleRate}));
    assertConfigQueryAnalyzerResponse(res, {mode, sampleRate});
    assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode, sampleRate);

    assert.commandWorked(conn.getDB(dbName).dropDatabase());
    if (st && isShardedColl) {
        assertNoQueryAnalyzerConfigDoc(conn, ns);
    } else {
        // TODO (SERVER-76443): Make sure that dropDatabase on replica set delete the
        // config.queryAnalyzers docs for all collections in the database being dropped.
        assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode, sampleRate);
    }
}

{
    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    testPersistingConfiguration(st.s);
    for (let isShardedColl of [true, false]) {
        testConfigurationDeletionDropCollection(st.s, {st, isShardedColl});
        testConfigurationDeletionDropDatabase(st.s, {st, isShardedColl});
    }

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    testPersistingConfiguration(primary);
    testConfigurationDeletionDropCollection(primary, {rst});
    testConfigurationDeletionDropDatabase(primary, {rst});

    rst.stopSet();
}
})();
