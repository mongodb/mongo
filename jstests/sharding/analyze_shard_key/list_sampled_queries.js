/**
 * Tests that listSampledQueries correctly returns sampled queries for both sharded clusters and
 * replica sets.
 *
 * @tags: [requires_fcv_70]
 */

(function() {
'use strict';

load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");
load("jstests/sharding/analyze_shard_key/libs/sampling_current_op_and_server_status_common.js");

const sampleRate = 10000;

const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const queryAnalysisWriterIntervalSecs = 1;

const mongodSetParameterOpts = {
    queryAnalysisSamplerConfigurationRefreshSecs,
    queryAnalysisWriterIntervalSecs,
};
const mongosSetParameterOpts = {
    queryAnalysisSamplerConfigurationRefreshSecs,
};

function insertDocuments(collection, numDocs) {
    const bulk = collection.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({x: i});
    }
    assert.commandWorked(bulk.execute());
}

function runTest(conn, {rst, st}) {
    assert(rst || st);
    assert(!rst || !st);

    const dbName = "testDb";
    const collName0 = "testColl0";
    const collName1 = "testColl1";
    const ns0 = dbName + "." + collName0;
    const ns1 = dbName + "." + collName1;
    const numDocs = 100;

    const adminDb = conn.getDB("admin");
    const testDb = conn.getDB(dbName);
    const collection0 = testDb.getCollection(collName0);
    const collection1 = testDb.getCollection(collName1);

    if (st) {
        // Shard collection1 and move one chunk to shard1.
        assert.commandWorked(conn.adminCommand({enableSharding: dbName}));
        st.ensurePrimaryShard(dbName, st.shard0.name);
        assert.commandWorked(testDb.runCommand(
            {createIndexes: collName1, indexes: [{key: {x: 1}, name: "xIndex"}]}));
        assert.commandWorked(conn.adminCommand({shardCollection: ns1, key: {x: 1}}));
        assert.commandWorked(conn.adminCommand({split: ns1, middle: {x: 0}}));
        assert.commandWorked(
            conn.adminCommand({moveChunk: ns1, find: {x: 0}, to: st.shard1.shardName}));
    }

    insertDocuments(collection0, numDocs);
    insertDocuments(collection1, numDocs);
    const collUuid0 = QuerySamplingUtil.getCollectionUuid(testDb, collName0);
    const collUuid1 = QuerySamplingUtil.getCollectionUuid(testDb, collName1);

    conn.adminCommand({configureQueryAnalyzer: ns0, mode: "full", sampleRate});
    conn.adminCommand({configureQueryAnalyzer: ns1, mode: "full", sampleRate});
    QuerySamplingUtil.waitForActiveSampling(ns0, collUuid0, {rst, st});
    QuerySamplingUtil.waitForActiveSampling(ns1, collUuid1, {rst, st});

    // Create read samples on collection0.
    let expectedSamples = [];
    assert.commandWorked(
        testDb.runCommand({aggregate: collName0, pipeline: [{$match: {x: 1}}], cursor: {}}));
    expectedSamples["aggregate"] = {
        ns: ns0,
        collectionUuid: collUuid0,
        cmdName: "aggregate",
        cmd: {filter: {x: 1}, collation: {locale: "simple"}}
    };
    assert.commandWorked(testDb.runCommand({count: collName0, query: {x: -1}}));
    expectedSamples["count"] =
        {ns: ns0, collectionUuid: collUuid0, cmdName: "count", cmd: {filter: {x: -1}}};
    assert.commandWorked(testDb.runCommand({distinct: collName0, key: "x", query: {x: 2}}));
    expectedSamples["distinct"] =
        {ns: ns0, collectionUuid: collUuid0, cmdName: "distinct", cmd: {filter: {x: 2}}};
    assert.commandWorked(testDb.runCommand({find: collName0, filter: {x: -3}, collation: {}}));
    expectedSamples["find"] = {
        ns: ns0,
        collectionUuid: collUuid0,
        cmdName: "find",
        cmd: {filter: {x: -3}, collation: {}}
    };

    // Create write samples on collection1.
    const updateCmdObj = {
        update: collName1,
        updates: [{q: {x: 4}, u: [{$set: {y: 1}}], multi: false}]
    };
    assert.commandWorked(testDb.runCommand(updateCmdObj));
    expectedSamples["update"] = {
        ns: ns1,
        collectionUuid: collUuid1,
        cmdName: "update",
        cmd: Object.assign({}, updateCmdObj, {$db: dbName})
    };
    const findAndModifyCmdObj =
        {findAndModify: collName1, query: {x: 5}, sort: {x: 1}, update: {$set: {z: 1}}};
    assert.commandWorked(testDb.runCommand(findAndModifyCmdObj));
    expectedSamples["findAndModify"] = {
        ns: ns1,
        collectionUuid: collUuid1,
        cmdName: "findAndModify",
        cmd: Object.assign({}, findAndModifyCmdObj, {$db: dbName})
    };
    const deleteCmdObj = {delete: collName1, deletes: [{q: {x: -6}, limit: 1}]};
    assert.commandWorked(testDb.runCommand(deleteCmdObj));
    expectedSamples["delete"] = {
        ns: ns1,
        collectionUuid: collUuid1,
        cmdName: "delete",
        cmd: Object.assign({}, deleteCmdObj, {$db: dbName})
    };

    // Verify samples on both collections.
    let response;
    assert.soon(() => {
        response = assert.commandWorked(adminDb.runCommand(
            {aggregate: 1, pipeline: [{$listSampledQueries: {}}, {$sort: {ns: 1}}], cursor: {}}));
        return response.cursor.firstBatch.length == 7;
    });
    let samples = response.cursor.firstBatch;
    samples.forEach((sample) => {
        AnalyzeShardKeyUtil.validateSampledQueryDocument(sample);
        QuerySamplingUtil.assertSubObject(sample, expectedSamples[sample.cmdName]);
    });

    // Verify that listing for collection0 returns only collection0 samples.
    assert.soon(() => {
        response = assert.commandWorked(adminDb.runCommand(
            {aggregate: 1, pipeline: [{$listSampledQueries: {namespace: ns0}}], cursor: {}}));
        return response.cursor.firstBatch.length == 4;
    });
    samples = response.cursor.firstBatch;
    samples.forEach((sample) => {
        AnalyzeShardKeyUtil.validateSampledQueryDocument(sample);
        QuerySamplingUtil.assertSubObject(sample, expectedSamples[sample.cmdName]);
    });

    // Verify that listing for collection1 returns only collection1 samples.
    assert.soon(() => {
        response = assert.commandWorked(adminDb.runCommand(
            {aggregate: 1, pipeline: [{$listSampledQueries: {namespace: ns1}}], cursor: {}}));
        return response.cursor.firstBatch.length == 3;
    });
    samples = response.cursor.firstBatch;
    samples.forEach((sample) => {
        AnalyzeShardKeyUtil.validateSampledQueryDocument(sample);
        QuerySamplingUtil.assertSubObject(sample, expectedSamples[sample.cmdName]);
    });

    // Verify that running on a database other than "admin" results in error.
    assert.commandFailedWithCode(
        testDb.runCommand({aggregate: 1, pipeline: [{$listSampledQueries: {}}], cursor: {}}),
        ErrorCodes.InvalidNamespace);
}

{
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        rs: {nodes: 2, setParameter: mongodSetParameterOpts},
        mongosOptions: {setParameter: mongosSetParameterOpts}
    });

    runTest(st.s, {st});

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: mongodSetParameterOpts}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    runTest(primary, {rst});

    rst.stopSet();
}

// Test that running the listSampledQueries aggregation stage is not allowed in multitenant
// replica sets.
if (!TestData.auth) {
    const rst = new ReplSetTest({
        name: jsTest.name() + "_multitenant",
        nodes: 2,
        nodeOptions: {setParameter: {multitenancySupport: true}}
    });
    rst.startSet({keyFile: "jstests/libs/key1"});
    rst.initiate();
    const primary = rst.getPrimary();
    const adminDb = primary.getDB("admin");

    assert.commandWorked(
        adminDb.runCommand({createUser: "user_monitor", pwd: "pwd", roles: ["clusterMonitor"]}));
    assert(adminDb.auth("user_monitor", "pwd"));
    assert.commandFailedWithCode(
        adminDb.runCommand({aggregate: 1, pipeline: [{$listSampledQueries: {}}], cursor: {}}),
        ErrorCodes.IllegalOperation);
    assert(adminDb.logout());

    rst.stopSet();
}
})();
