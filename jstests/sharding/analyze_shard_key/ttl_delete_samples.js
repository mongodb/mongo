/**
 * Tests for the query sampling and timely deletion of sampled query and diff documents.
 *
 * @tags: [requires_fcv_62, featureFlagAnalyzeShardKey]
 */

(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const st = new ShardingTest({
    shards: 1,
    rs: {
        nodes: 2,
        setParameter: {
            queryAnalysisWriterIntervalSecs: 1,
            queryAnalysisSampleExpirationSecs: 2,
        }
    },
    mongosOptions: {
        setParameter: {
            queryAnalysisSamplerConfigurationRefreshSecs: 1,
        }
    },
});

const dbName = "testDb";
const collName = "testQuerySampling";
const ns = dbName + "." + collName;
const kNumDocs = 20;

const mydb = st.s.getDB(dbName);
const coll = mydb.getCollection(collName);

// Insert documents
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < kNumDocs; i++) {
    bulk.insert({x: i, y: i});
}
assert.commandWorked(bulk.execute());

// Enable query sampling
assert.commandWorked(
    st.s.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 1000}));

QuerySamplingUtil.waitForActiveSampling(st.s);

// Find each document
for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(mydb.runCommand({find: collName, filter: {x: i}}));
}

// Update each document
for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(mydb.runCommand({
        update: collName,
        updates: [{q: {x: i}, u: [{$set: {y: i + 1}}], multi: false, upsert: false}]
    }));
}

// Assert that query sample documents exist
let numQueryDocs = 0;
let numDiffDocs = 0;
let prevNumQueryDocs = -1;
let prevNumDiffDocs = -1;
assert.soon(() => {
    numQueryDocs = QuerySamplingUtil.getNumSampledQueryDocuments(st);
    numDiffDocs = QuerySamplingUtil.getNumSampledQueryDiffDocuments(st);
    return numQueryDocs > 0 && numDiffDocs > 0;
});

printjson({"numQueryDocs": numQueryDocs, "numDiffDocs": numDiffDocs});

mydb.adminCommand({setParameter: 1, ttlMonitorSleepSecs: 1});

// Assert that query sample documents have been deleted
assert.soon(() => {
    return (QuerySamplingUtil.getNumSampledQueryDocuments(st) == 0 &&
            QuerySamplingUtil.getNumSampledQueryDiffDocuments(st) == 0);
});

st.stop();
})();
