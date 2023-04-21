/**
 * Tests for the query sampling and timely deletion of sampled query and diff documents.
 *
 * @tags: [requires_fcv_70]
 */

(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const queryAnalysisWriterIntervalSecs = 1;
// To speed up the test, make the sampled query documents expire right away. To prevent the
// documents from being deleted before the count is verified, make the TTL monitor have a large
// sleep interval at first and then lower it at the end of the test when verifying that the
// documents do get deleted by the TTL monitor.
const queryAnalysisSampleExpirationSecs = 1;
const ttlMonitorSleepSecs = 3600;

const st = new ShardingTest({
    shards: 1,
    rs: {
        nodes: 2,
        setParameter: {
            queryAnalysisSamplerConfigurationRefreshSecs,
            queryAnalysisWriterIntervalSecs,
            queryAnalysisSampleExpirationSecs,
            ttlMonitorSleepSecs,
            logComponentVerbosity: tojson({sharding: 2})
        }
    },
    mongosOptions: {
        setParameter: {
            queryAnalysisSamplerConfigurationRefreshSecs,
        }
    },
});
const shard0Primary = st.rs0.getPrimary();

const dbName = "testDb";
const collName = "testQuerySampling";
const ns = dbName + "." + collName;
const kNumDocs = 20;

const testDB = st.s.getDB(dbName);
const testColl = testDB.getCollection(collName);

// Insert documents
const bulk = testColl.initializeUnorderedBulkOp();
for (let i = 0; i < kNumDocs; i++) {
    bulk.insert({x: i, y: i});
}
assert.commandWorked(bulk.execute());
const collUuid = QuerySamplingUtil.getCollectionUuid(testDB, collName);

// Enable query sampling
assert.commandWorked(
    st.s.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 1000}));
QuerySamplingUtil.waitForActiveSamplingShardedCluster(st, ns, collUuid);

// Find each document
for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(testDB.runCommand({find: collName, filter: {x: i}}));
}

// Update each document
for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(testDB.runCommand({
        update: collName,
        updates: [{q: {x: i}, u: [{$set: {y: i + 1}}], multi: false, upsert: false}]
    }));
}

// Assert that query sample documents exist
let numQueryDocs = 0;
let numDiffDocs = 0;
assert.soon(() => {
    numQueryDocs = QuerySamplingUtil.getNumSampledQueryDocuments(st);
    numDiffDocs = QuerySamplingUtil.getNumSampledQueryDiffDocuments(st);
    return numQueryDocs > 0 && numDiffDocs > 0;
});
printjson({"numQueryDocs": numQueryDocs, "numDiffDocs": numDiffDocs});

// Lower the TTL monitor sleep interval.
assert.commandWorked(shard0Primary.adminCommand({setParameter: 1, ttlMonitorSleepSecs: 1}));

// Assert that query sample documents are eventually deleted.
assert.soon(() => {
    return (QuerySamplingUtil.getNumSampledQueryDocuments(st) == 0 &&
            QuerySamplingUtil.getNumSampledQueryDiffDocuments(st) == 0);
});

st.stop();
})();
