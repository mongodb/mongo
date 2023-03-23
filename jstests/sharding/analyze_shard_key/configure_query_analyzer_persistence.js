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

/**
 * Returns test cases for all combinations of options for the configureQueryAnalyzer command.
 */
function makeTestCases() {
    const testCases = [];
    for (const mode of ["off", "full"]) {
        for (const sampleRate of [null, -1.0, 0.0, 0.2]) {
            let testCase = Object.assign({}, {command: {mode, sampleRate}});
            if (sampleRate == null) {
                delete testCase.command.sampleRate;
            }
            if ((mode == "off" && sampleRate !== null) ||
                (mode == "full" &&
                 (sampleRate == null || typeof sampleRate !== "number" || sampleRate <= 0.0))) {
                continue;  // These cases are tested in configure_query_analyzer_basic.js.
            }
            testCases.push(testCase);
        }
    }
    return testCases;
}

function assertConfigQueryAnalyzerResponse(res, mode, sampleRate) {
    assert.eq(res.mode, mode);
    assert.eq(res.sampleRate, sampleRate);
}

function assertQueryAnalyzerConfigDoc(conn, dbName, collName, mode, sampleRate) {
    const listCollRes = assert.commandWorked(
        conn.getDB(dbName).runCommand({listCollections: 1, filter: {name: collName}}));
    const uuid = listCollRes.cursor.firstBatch[0].info.uuid;
    const doc = conn.getCollection("config.queryAnalyzers").findOne({_id: uuid});
    assert.eq(doc.mode, mode, doc);
    if (mode == "off") {
        assert.eq(doc.hasOwnProperty("sampleRate"), false, doc);
    } else if (mode == "full") {
        assert.eq(doc.sampleRate, sampleRate, doc);
    }
}

function assertNoQueryAnalyzerConfigDoc(conn, dbName, collName) {
    const ns = dbName + "." + collName;
    const doc = conn.getCollection("config.queryAnalyzers").findOne({ns: ns});
    assert.eq(doc, null, doc);
}

function testPersistingConfiguration(conn, testCases) {
    const dbName = "testDb-" + extractUUIDFromObject(UUID());
    const collName = "testColl";
    const ns = dbName + "." + collName;

    assert.commandWorked(conn.getDB(dbName).runCommand({insert: collName, documents: [{x: 1}]}));

    testCases.forEach(testCase => {
        jsTest.log(
            `Testing that the configureQueryAnalyzer command persists the configuration correctly ${
                tojson(testCase)}`);

        const res = conn.adminCommand({
            configureQueryAnalyzer: ns,
            mode: testCase.command.mode,
            sampleRate: testCase.command.sampleRate
        });
        assert.commandWorked(res);
        assertConfigQueryAnalyzerResponse(res, testCase.command.mode, testCase.command.sampleRate);
        assertQueryAnalyzerConfigDoc(
            conn, dbName, collName, testCase.command.mode, testCase.command.sampleRate);
    });
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

    const mode = "full";
    const sampleRate = 0.5;
    const res =
        assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode, sampleRate}));
    assertConfigQueryAnalyzerResponse(res, mode, sampleRate);
    assertQueryAnalyzerConfigDoc(conn, dbName, collName, mode, sampleRate);

    if (dropDatabase) {
        assert.commandWorked(db.dropDatabase());
    } else if (dropCollection) {
        assert(coll.drop());
    }

    assertNoQueryAnalyzerConfigDoc(conn, dbName, collName);
}

const testCases = makeTestCases();

{
    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    testPersistingConfiguration(st.s, testCases);
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

    testPersistingConfiguration(primary, testCases);
    // TODO (SERVER-70479): Make sure that dropDatabase and dropCollection delete the
    // config.queryAnalyzers doc for the collection being dropped.
    // testDeletingConfigurations(primary, {dropDatabase: true, isShardedColl: false});
    // testDeletingConfigurations(primary, {dropCollection: true, isShardedColl: false});

    rst.stopSet();
}
})();
