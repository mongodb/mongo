/**
 * Tests that the configureQueryAnalyzer command persists the configuration in a document
 * in config.queryAnalyzers and that the document is deleted when the associated collection
 * is dropped.
 *
 * @tags: [requires_fcv_62, featureFlagAnalyzeShardKey]
 */

(function() {
"use strict";

const dbName = "testDb";

/**
 * TestCase: {
 *    command: {ns : "coll namespace",
 *              mode : "full"|"off",
 *              sampleRate : 1.2},
 * }
 *
 */

/**
 * Create documents represting all combinations of options for configureQueryAnalyzer command.
 * @returns array of documents
 */
function optionsAllCombinations() {
    const testCases = [];
    const collName = "collection";
    for (const mode of ["off", "full"]) {
        for (const sampleRate of [null, -1.0, 0.0, 0.2]) {
            let testCase =
                Object.assign({}, {command: {ns: dbName + "." + collName, mode, sampleRate}});
            if (sampleRate == null) {
                delete testCase.command.sampleRate;
            }
            if ((mode == "off" && sampleRate !== null) ||
                (mode == "full" &&
                 (sampleRate == null || typeof sampleRate !== "number" || sampleRate <= 0.0))) {
                continue;  // These cases are tested in configuer_query_analyzer_basic.js.
            }
            testCases.push(testCase);
        }
    }
    return testCases;
}

function assertConfigQueryAnalyzerResponse(res, mode, sampleRate) {
    assert.eq(res.ok, 1);
    assert.eq(res.mode, mode);
    assert.eq(res.sampleRate, sampleRate);
}

function assertQueryAnalyzerConfigDoc(configDb, db, collName, mode, sampleRate) {
    const configColl = configDb.getCollection('queryAnalyzers');
    const listCollRes =
        assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
    const uuid = listCollRes.cursor.firstBatch[0].info.uuid;
    const doc = configColl.findOne({_id: uuid});
    assert.eq(doc.mode, mode, doc);
    if (mode == "off") {
        assert.eq(doc.hasOwnProperty("sampleRate"), false, doc);
    } else if (mode == "full") {
        assert.eq(doc.sampleRate, sampleRate, doc);
    }
}

function assertNoQueryAnalyzerConfigDoc(configDb, db, collName) {
    const configColl = configDb.getCollection('queryAnalyzers');
    const ns = db.getName() + "." + collName;
    const doc = configColl.findOne({ns: ns});
    assert.eq(doc, null, doc);
}

function testConfigurationOptions(conn, testCases) {
    const collName = "collection";
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);
    const coll = db.getCollection(ns);
    let config = conn.getDB('config');
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(db.runCommand({insert: collName, documents: [{x: 1}]}));

    testCases.forEach(testCase => {
        jsTest.log(`Running configureQueryAnalyzer command on test case ${tojson(testCase)}`);

        const res = conn.adminCommand({
            configureQueryAnalyzer: testCase.command.ns,
            mode: testCase.command.mode,
            sampleRate: testCase.command.sampleRate
        });
        assert.commandWorked(res);
        assertConfigQueryAnalyzerResponse(res, testCase.command.mode, testCase.command.sampleRate);
        assertQueryAnalyzerConfigDoc(
            config, db, collName, testCase.command.mode, testCase.command.sampleRate);
    });
}

function testDropCollectionDeletesConfig(conn) {
    const db = conn.getDB(dbName);

    const collNameSh = "collection2DropSh";
    const nsSh = dbName + "." + collNameSh;
    const collSh = db.getCollection(collNameSh);
    const collNameUnsh = "collection2DropUnsh";
    const nsUnsh = dbName + "." + collNameUnsh;
    const collUnsh = db.getCollection(collNameUnsh);

    const config = conn.getDB('config');
    const shardKey = {skey: 1};
    const shardKeySplitPoint = {skey: 2};

    jsTest.log('Testing drop collection deletes query analyzer config doc');

    assert.commandWorked(conn.adminCommand({shardCollection: nsSh, key: shardKey}));
    assert.commandWorked(conn.adminCommand({split: nsSh, middle: shardKeySplitPoint}));

    assert.commandWorked(db.runCommand({insert: collNameSh, documents: [{skey: 1, y: 1}]}));
    assert.commandWorked(db.runCommand({insert: collNameUnsh, documents: [{skey: 1, y: 1}]}));

    // sharded collection

    const mode = "full";
    const sampleRate = 0.5;
    const resSh =
        conn.adminCommand({configureQueryAnalyzer: nsSh, mode: mode, sampleRate: sampleRate});
    assert.commandWorked(resSh);
    assertConfigQueryAnalyzerResponse(resSh, mode, sampleRate);
    assertQueryAnalyzerConfigDoc(config, db, collNameSh, mode, sampleRate);

    collSh.drop();
    assertNoQueryAnalyzerConfigDoc(config, db, collNameSh);

    // unsharded collection

    const resUnsh =
        conn.adminCommand({configureQueryAnalyzer: nsUnsh, mode: mode, sampleRate: sampleRate});
    assert.commandWorked(resUnsh);
    assertConfigQueryAnalyzerResponse(resUnsh, mode, sampleRate);
    assertQueryAnalyzerConfigDoc(config, db, collNameUnsh, mode, sampleRate);

    collUnsh.drop();
    assertNoQueryAnalyzerConfigDoc(config, db, collNameUnsh);
}

function testDropDatabaseDeletesConfig(conn) {
    let db = conn.getDB(dbName);
    const collNameSh = "collection2DropSh";
    const nsSh = dbName + "." + collNameSh;
    const collSh = db.getCollection(nsSh);

    const config = conn.getDB('config');
    const shardKey = {skey: 1};
    const shardKeySplitPoint = {skey: 2};

    jsTest.log('Testing drop database deletes query analyzer config doc');
    assert.commandWorked(conn.adminCommand({shardCollection: nsSh, key: shardKey}));
    assert.commandWorked(conn.adminCommand({split: nsSh, middle: shardKeySplitPoint}));
    assert.commandWorked(db.runCommand({insert: collNameSh, documents: [{skey: 1, y: 1}]}));

    // sharded collection

    const mode = "full";
    const sampleRate = 0.5;
    const resSh =
        conn.adminCommand({configureQueryAnalyzer: nsSh, mode: mode, sampleRate: sampleRate});
    assert.commandWorked(resSh);
    assertConfigQueryAnalyzerResponse(resSh, mode, sampleRate);
    assertQueryAnalyzerConfigDoc(config, db, collNameSh, mode, sampleRate);
    db.dropDatabase();
    assertNoQueryAnalyzerConfigDoc(config, db, collNameSh);

    // unsharded collection

    db = conn.getDB(dbName);
    const collNameUnsh = "collection2DropUnsh";
    const nsUnsh = dbName + "." + collNameUnsh;
    const collUnsh = db.getCollection(nsUnsh);
    assert.commandWorked(db.runCommand({insert: collNameUnsh, documents: [{skey: 1, y: 1}]}));

    const resUnsh =
        conn.adminCommand({configureQueryAnalyzer: nsUnsh, mode: mode, sampleRate: sampleRate});
    assert.commandWorked(resUnsh);
    assertConfigQueryAnalyzerResponse(resUnsh, mode, sampleRate);
    assertQueryAnalyzerConfigDoc(config, db, collNameUnsh, mode, sampleRate);
    db.dropDatabase();
    // TODO (SERVER-70479): dropDatabase doesn't delete the config.queryAnalyzers docs for unsharded
    // collections.
    // assertNoQueryAnalyzerConfigDoc(config, db, collNameUnsh);
}

{
    const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name});

    const AllTestCases = optionsAllCombinations();
    testConfigurationOptions(st.s, AllTestCases);

    testDropCollectionDeletesConfig(st.s);
    testDropDatabaseDeletesConfig(st.s);

    st.stop();
}
})();
