/**
 * Tests that inserts related to query sampling are deprioritized.
 *
 * @tags: [requires_fcv_71]
 */

(function() {

"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const sampleRate = 1000;
const queryAnalysisWriterIntervalSecs = 1;
const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const mongodSetParameterOpts = {
    queryAnalysisWriterIntervalSecs,
};
const mongosSetParameterOpts = {
    queryAnalysisSamplerConfigurationRefreshSecs,
};

function runTest(conn, primary) {
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

    assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate}));
    QuerySamplingUtil.waitForActiveSampling(conn);

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
    runTest(st.s, st.rs0.getPrimary());

    st.stop();
}
{
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: mongodSetParameterOpts}});
    rst.startSet();
    rst.initiate();

    jsTest.log("Testing deprioritized insert in replica set.");
    runTest(rst.getPrimary(), rst.getPrimary());

    rst.stopSet();
}
})();
