/**
 * Tests that inserts related to query sampling are deprioritized.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   featureFlagDeprioritizeLowPriorityOperations,
 *   requires_fcv_70,
 * ]
 * TODO SERVER-79365 Remove multiversion_incompatible tag for v8.0. This test cannot run
 * in multiversion tests involving v7.0 binaries because the flag
 * featureFlagDeprioritizeLowPriorityOperations defaults to false in v7.0 but does not exist
 * in v7.1. Therefore, resmoke in master and v7.1 is unable to set this flag for v7.0 in
 * all-feature-flags variants.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {QuerySamplingUtil} from "jstests/sharding/analyze_shard_key/libs/query_sampling_util.js";

const samplesPerSecond = 1000;
const queryAnalysisWriterIntervalSecs = 1;
const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const mongodSetParameterOpts = {
    queryAnalysisWriterIntervalSecs,
};
const mongosSetParameterOpts = {
    queryAnalysisSamplerConfigurationRefreshSecs,
};

function runTest(conn, primary, {st, rst}) {
    const dbName = "testDb";
    const collName = "testColl";
    const ns = dbName + "." + collName;
    const testDb = conn.getDB(dbName);
    const testColl = testDb.getCollection(collName);
    const sampleCollName = "sampledQueries";
    const sampleDiffCollName = "sampledQueriesDiff";
    const sampleNs = "config." + sampleCollName;
    const sampleDiffNs = "config." + sampleDiffCollName;

    assert.commandWorked(testColl.insert([{x: 1}]));
    const collUuid = QuerySamplingUtil.getCollectionUuid(testDb, collName);

    assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", samplesPerSecond}));
    QuerySamplingUtil.waitForActiveSampling(ns, collUuid, {st, rst});

    // Test insert to config.sampledQueries.
    const fp1 = configureFailPoint(primary, "hangInsertBeforeWrite", {ns: sampleNs});
    assert.commandWorked(testDb.runCommand({find: collName, filter: {x: 1}}));
    // Wait for the sampling buffer to be flushed.
    fp1.wait();

    let currentOpDocs =
        conn.getDB("admin")
            .aggregate(
                [{$currentOp: {allUsers: true}}, {$match: {"command.insert": sampleCollName}}])
            .toArray();
    assert.eq(currentOpDocs.length, 1, tojson(currentOpDocs));
    assert.eq(currentOpDocs[0]["admissionPriority"], "low", tojson(currentOpDocs[0]));

    fp1.off();

    // Test insert to config.sampledQueriesDiff.
    const fp2 = configureFailPoint(primary, "hangInsertBeforeWrite", {ns: sampleDiffNs});
    assert.commandWorked(
        testDb.runCommand({update: collName, updates: [{q: {x: 1}, u: {$set: {y: 1}}}]}));
    // Wait for the sampling buffer to be flushed.
    fp2.wait();

    currentOpDocs =
        conn.getDB("admin")
            .aggregate(
                [{$currentOp: {allUsers: true}}, {$match: {"command.insert": sampleDiffCollName}}])
            .toArray();
    assert.eq(currentOpDocs.length, 1, tojson(currentOpDocs));
    assert.eq(currentOpDocs[0]["admissionPriority"], "low", tojson(currentOpDocs[0]));

    fp2.off();
}

{
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        rs: {nodes: 2, setParameter: mongodSetParameterOpts},
        mongosOptions: {setParameter: mongosSetParameterOpts}
    });

    jsTest.log("Testing deprioritized insert in sharded cluster.");
    runTest(st.s, st.rs0.getPrimary(), {st});

    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove block
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: mongodSetParameterOpts}});
    rst.startSet();
    rst.initiate();

    jsTest.log("Testing deprioritized insert in replica set.");
    runTest(rst.getPrimary(), rst.getPrimary(), {rst});

    rst.stopSet();
}
