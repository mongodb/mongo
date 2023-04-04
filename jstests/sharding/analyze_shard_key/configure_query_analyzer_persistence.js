/**
 * Tests that the configureQueryAnalyzer command persists the configuration in a document
 * in config.queryAnalyzers and that the document is deleted when the associated collection
 * is dropped.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */

(function() {
"use strict";

load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

function assertConfigQueryAnalyzerResponse(res, newConfig, oldConfig) {
    assert.eq(res.newConfiguration, newConfig, res);
    if (oldConfig) {
        assert.eq(res.oldConfiguration, oldConfig);
    } else {
        assert(!res.hasOwnProperty("oldConfiguration"), oldConfig);
    }
}

function assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode, sampleRate, startTime, stopTime) {
    const doc = conn.getCollection("config.queryAnalyzers").findOne({_id: collUuid});
    assert.eq(doc.ns, ns, doc);
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

function assertNoQueryAnalyzerConfigDoc(conn, collUuid) {
    const doc = conn.getCollection("config.queryAnalyzers").findOne({_id: collUuid});
    assert.eq(doc, null, doc);
}

function testPersistingConfiguration(conn) {
    const dbName = "testDb-" + extractUUIDFromObject(UUID());
    const collName = "testColl";
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);

    assert.commandWorked(db.createCollection(collName));
    const collUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    jsTest.log(
        `Testing that the configureQueryAnalyzer command persists the configuration correctly ${
            tojson({dbName, collName, collUuid})}`);

    // Run a configureQueryAnalyzer command to disable query sampling. Verify that the command
    // fails since query sampling is not even active.
    const mode0 = "off";
    assert.commandFailedWithCode(conn.adminCommand({configureQueryAnalyzer: ns, mode: mode0}),
                                 ErrorCodes.IllegalOperation);
    assertNoQueryAnalyzerConfigDoc(conn, collUuid);

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

    // Run a configureQueryAnalyzer command to disable query sampling. Verify that the 'sampleRate'
    // doesn't get unset.
    const mode5 = "off";
    const res5 = assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode: mode5}));
    const doc5 =
        assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode5, sampleRate4, doc4.startTime);
    assertConfigQueryAnalyzerResponse(res5,
                                      {mode: mode5} /* newConfig */,
                                      {mode: mode4, sampleRate: sampleRate4} /* oldConfig */);

    // Retry the previous configureQueryAnalyzer command. Verify that the 'stopTime' remains the
    // same.
    assert.commandFailedWithCode(conn.adminCommand({configureQueryAnalyzer: ns, mode: mode5}),
                                 ErrorCodes.IllegalOperation);
    assertQueryAnalyzerConfigDoc(
        conn, ns, collUuid, mode5, sampleRate4, doc4.startTime, doc5.stopTime);
}

function testDeletingConfigurations(conn, {dropDatabase, dropCollection, isShardedColl, st}) {
    assert(dropDatabase || dropCollection, "Expected the test to drop the database or collection");
    assert(!dropDatabase || !dropCollection);
    assert(!isShardedColl || st);

    const dbName = "testDb-" + extractUUIDFromObject(UUID());
    const collName = isShardedColl ? "testCollSharded" : "testCollUnsharded";
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);
    jsTest.log(`Testing configuration deletion ${
        tojson({dbName, collName, isShardedColl, dropDatabase, dropCollection})}`);

    assert.commandWorked(db.createCollection(collName));
    if (isShardedColl) {
        assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
        st.ensurePrimaryShard(dbName, st.shard0.name);
        assert.commandWorked(st.s0.adminCommand({shardCollection: ns, key: {x: 1}}));
        assert.commandWorked(st.s0.adminCommand({split: ns, middle: {x: 0}}));
        assert.commandWorked(
            st.s0.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
    }
    const collUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    const mode = "full";
    const sampleRate = 0.5;
    const res =
        assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode, sampleRate}));
    assertConfigQueryAnalyzerResponse(res, {mode, sampleRate});
    assertQueryAnalyzerConfigDoc(conn, ns, collUuid, mode, sampleRate);

    if (dropDatabase) {
        assert.commandWorked(db.dropDatabase());
    } else if (dropCollection) {
        assert(coll.drop());
    }

    assertNoQueryAnalyzerConfigDoc(conn, collUuid);
}

{
    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    testPersistingConfiguration(st.s);
    // TODO (SERVER-70479): Make sure that dropDatabase and dropCollection delete the
    // config.queryAnalyzers doc for the collection being dropped.
    // testDeletingConfigurations(st.s, {dropDatabase: true, isShardedColl: false, st});
    testDeletingConfigurations(st.s, {dropDatabase: true, isShardedColl: true, st});
    testDeletingConfigurations(st.s, {dropCollection: true, isShardedColl: false, st});
    testDeletingConfigurations(st.s, {dropCollection: true, isShardedColl: true, st});

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    testPersistingConfiguration(primary);
    // TODO (SERVER-70479): Make sure that dropDatabase and dropCollection delete the
    // config.queryAnalyzers doc for the collection being dropped.
    // testDeletingConfigurations(primary, {dropDatabase: true, isShardedColl: false});
    // testDeletingConfigurations(primary, {dropCollection: true, isShardedColl: false});

    rst.stopSet();
}
})();
