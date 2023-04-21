/**
 * Tests that FCV downgrade causes a mongod to stop writing sampled queries and clear any sampled
 * queries in its write buffer.
 */

(function() {

'use strict';
load("jstests/libs/fail_point_util.js");
load("jstests/sharding/analyze_shard_key/libs/sampling_current_op_and_server_status_common.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const queryAnalysisWriterIntervalSecs = 1;
const mongodSetParameterOpts = {queryAnalysisWriterIntervalSecs};
const mongosSetParameterOpts = {
    queryAnalysisSamplerConfigurationRefreshSecs,
    queryAnalysisWriterIntervalSecs
};

const st = new ShardingTest({
    shards: 1,
    mongos: 2,
    mongosOptions: {setParameter: mongosSetParameterOpts},
    rs: {nodes: 1, setParameter: mongodSetParameterOpts},
});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

const sampleRate = 10000;
const numSampledQueries = 10;

const shard0Primary = st.rs0.getPrimary();
const adminDb = st.s.getDB("admin");
const configDb = st.s.getDB("config");
const testDb = st.s.getDB(dbName);
const testColl = testDb.getCollection(collName);

assert.commandWorked(testColl.insert([{x: 1}]));
const collUuid = QuerySamplingUtil.getCollectionUuid(testDb, collName);

assert.commandWorked(st.s0.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate}));
QuerySamplingUtil.waitForActiveSamplingShardedCluster(st, ns, collUuid);

assert.commandWorked(shard0Primary.adminCommand(
    {configureFailPoint: "disableQueryAnalysisWriterFlusher", mode: "alwaysOn"}));

for (let i = 0; i < numSampledQueries; i++) {
    assert.commandWorked(testDb.runCommand({find: collName, filter: {x: i}}));
}

// Wait for the samples to get added to the write buffer.
assert.soon(() => {
    const counterStatus = getCurrentOpAndServerStatusMongod(shard0Primary);
    return counterStatus.currentOp[0].sampledReadsCount == numSampledQueries;
});

const failpoint = configureFailPoint(shard0Primary, "disableQueryAnalysisWriterFlusher");

assert.commandWorked(adminDb.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

failpoint.off();

// Sleep for several flush intervals to ensure that the buffer is empty and that no sample query
// documents are written.
sleep(queryAnalysisWriterIntervalSecs * 1000 * 3);
let response = assert.commandWorked(configDb.runCommand({count: "sampledQueries"}));
assert.eq(response.n, 0, "number of samples");

assert.commandWorked(adminDb.runCommand({setFeatureCompatibilityVersion: latestFCV}));

// Sleep for several flush intervals to ensure that the buffer is empty and no sample query
// documents are written even if the FCV is set back to 'latestFCV'.
sleep(queryAnalysisWriterIntervalSecs * 1000 * 3);
response = assert.commandWorked(configDb.runCommand({count: "sampledQueries"}));
assert.eq(response.n, 0, "number of samples");

st.stop();
})();
