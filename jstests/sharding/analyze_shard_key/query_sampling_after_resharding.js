/**
 * Tests that resharding does not cause query sampling configuration for a collection to change.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {QuerySamplingUtil} from "jstests/sharding/analyze_shard_key/libs/query_sampling_util.js";

function setUpCollection(st, isShardedColl) {
    const dbName = "testDb-" + extractUUIDFromObject(UUID());
    if (st) {
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    }
    const collName = isShardedColl ? "testCollSharded" : "testCollUnsharded";
    const ns = dbName + "." + collName;
    const db = st.s.getDB(dbName);

    assert.commandWorked(db.createCollection(collName));
    if (isShardedColl) {
        assert(st);
        assert.commandWorked(st.s0.adminCommand({shardCollection: ns, key: {x: 1}}));
        assert.commandWorked(st.s0.adminCommand({split: ns, middle: {x: 0}}));
        assert.commandWorked(
            st.s0.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
    }

    return {dbName, collName};
}

function enableQuerySampling(st, dbName, collName) {
    const ns = dbName + "." + collName;
    const collUuid = QuerySamplingUtil.getCollectionUuid(st.s.getDB(dbName), collName);
    assert.commandWorked(st.s.adminCommand(
        {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: samplesPerSecond}));
    QuerySamplingUtil.waitForActiveSampling(ns, collUuid, {st});
}

function validateQueryAnalyzerDoc(conn, dbName, collName) {
    const mongosDB = conn.getDB(dbName);
    const ns = dbName + "." + collName;

    let queryAnalyzerDoc = conn.getCollection("config.queryAnalyzers").findOne({_id: ns});
    assert(queryAnalyzerDoc);

    let queryAnalyzerCollUuid = queryAnalyzerDoc.collUuid;
    let collUuid = QuerySamplingUtil.getCollectionUuid(mongosDB, collName);
    assert.eq(bsonWoCompare(queryAnalyzerCollUuid, collUuid), 0);

    assert.eq("full", queryAnalyzerDoc.mode);
    assert.eq(samplesPerSecond, queryAnalyzerDoc.samplesPerSecond);
}

function assertQuerySampling(dbName, collName, isActive, st) {
    const ns = dbName + "." + collName;
    const uuid = UUID();
    const conn = st.s;
    const mongosDB = conn.getDB(dbName);

    // Make sure that we wait for active sampling. Otherwise, the find query below will be discarded
    // due to either the collection UUID mismatch or a lack of tokens in the mongos.
    if (isActive) {
        let collUuid = QuerySamplingUtil.getCollectionUuid(mongosDB, collName);
        QuerySamplingUtil.waitForActiveSampling(ns, collUuid, {st});
    }

    assert.commandWorked(mongosDB.runCommand({find: collName, filter: {x: 0, comment: uuid}}));

    sleep(queryAnalysisWriterIntervalSecs * 1000);

    // If query sampling is active, assert that the find query above was sampled. Otherwise, assert
    // that it was not.
    assert.soon(() => {
        const aggRes = assert.commandWorked(conn.adminCommand(
            {aggregate: 1, pipeline: [{$listSampledQueries: {namespace: ns}}], cursor: {}}));
        const sampledQueryDocs = aggRes.cursor.firstBatch;

        const matchCount =
            sampledQueryDocs.filter(doc => bsonWoCompare(uuid, doc.cmd.filter.comment) == 0).length;

        if (isActive) {
            return matchCount == 1;
        } else {
            return matchCount == 0;
        }
    });
}

function testMoveCollectionQuerySamplingEnabled(st) {
    assert(st);

    const {dbName, collName} = setUpCollection(st, false /* isShardedColl */);

    const ns = dbName + "." + collName;
    const srcShard = st.shard0.shardName;
    const dstShard = st.shard1.shardName;
    const conn = st.s;

    jsTest.log(`Testing moveCollection ${tojson({dbName, collName, srcShard, dstShard})}`);

    enableQuerySampling(st, dbName, collName);

    validateQueryAnalyzerDoc(conn, dbName, collName);

    // Move the collection to the destination shard.
    assert.commandWorked(st.s.adminCommand({moveCollection: ns, toShard: dstShard}));

    validateQueryAnalyzerDoc(conn, dbName, collName);
    assertQuerySampling(dbName, collName, true /* isActive */, st);
}

function testReshardCollectionQuerySamplingEnabled(st) {
    assert(st);

    const {dbName, collName} = setUpCollection(st, true /* isShardedColl */);

    const ns = dbName + "." + collName;
    const srcShard = st.shard0.shardName;
    const dstShard = st.shard1.shardName;
    const conn = st.s;

    jsTest.log(`Testing reshardCollection ${tojson({dbName, collName, srcShard, dstShard})}`);

    enableQuerySampling(st, dbName, collName);

    validateQueryAnalyzerDoc(conn, dbName, collName);

    // Insert documents with field "a" into sharded collection so that we can successfully reshard
    // with this key
    for (let i = 0; i < 1000; i++) {
        st.s.getCollection(ns).insert({a: i});
    }

    // Reshard the sharded collection.
    assert.commandWorked(st.s.adminCommand({reshardCollection: ns, key: {a: 1}}));

    validateQueryAnalyzerDoc(conn, dbName, collName);
    assertQuerySampling(dbName, collName, true /* isActive */, st);
}

function testUnshardCollectionQuerySamplingEnabled(st) {
    assert(st);

    const {dbName, collName} = setUpCollection(st, true /* isShardedColl */);

    const ns = dbName + "." + collName;
    const srcShard = st.shard0.shardName;
    const dstShard = st.shard1.shardName;
    const conn = st.s;

    jsTest.log(`Testing unshardCollection ${tojson({dbName, collName, srcShard, dstShard})}`);

    enableQuerySampling(st, dbName, collName);

    validateQueryAnalyzerDoc(conn, dbName, collName);

    // Unshard the sharded collection.
    assert.commandWorked(st.s.adminCommand({unshardCollection: ns}));

    validateQueryAnalyzerDoc(conn, dbName, collName);
    assertQuerySampling(dbName, collName, true /* isActive */, st);
}

/**
 * Tests that resharding does not enable query sampling.
 */
function testReshardQuerySamplingDisabled(st, isShardedColl) {
    assert(st);

    const {dbName, collName} = setUpCollection(st, isShardedColl);
    const ns = dbName + "." + collName;
    const dstShard = st.shard1.shardName;
    const conn = st.s;

    if (isShardedColl) {
        for (let i = 0; i < 1000; i++) {
            st.s.getCollection(ns).insert({a: i});
        }

        assert.commandWorked(st.s.adminCommand({reshardCollection: ns, key: {a: 1}}));

        assertQuerySampling(dbName, collName, false /* isActive */, st);

        assert.commandWorked(st.s.adminCommand({unshardCollection: ns}));
    } else {
        assert.commandWorked(st.s.adminCommand({moveCollection: ns, toShard: dstShard}));
    }

    assertQuerySampling(dbName, collName, false /* isActive */, st);

    let queryAnalyzerDoc = conn.getCollection("config.queryAnalyzers").findOne({_id: ns});
    assert(!queryAnalyzerDoc);
}

const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const queryAnalysisWriterIntervalSecs = 1;
const samplesPerSecond = 1000;

const mongodSetParameterOpts = {
    queryAnalysisSamplerConfigurationRefreshSecs,
    queryAnalysisWriterIntervalSecs,
    logComponentVerbosity: tojson({sharding: 2}),
};
const mongosSetParametersOpts = {
    queryAnalysisSamplerConfigurationRefreshSecs,
    logComponentVerbosity: tojson({sharding: 3})
};

{
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 1, setParameter: mongodSetParameterOpts},
        mongosOptions: {setParameter: mongosSetParametersOpts}
    });

    testMoveCollectionQuerySamplingEnabled(st);
    testUnshardCollectionQuerySamplingEnabled(st);
    testReshardCollectionQuerySamplingEnabled(st);

    for (let isShardedColl of [true, false]) {
        testReshardQuerySamplingDisabled(st, isShardedColl);
    }

    st.stop();
}
