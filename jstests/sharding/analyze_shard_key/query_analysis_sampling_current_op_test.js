/**
 * Test "query analyzer" section of currentOp command during query sampling.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */

(function() {
"use strict";

// load("jstests/libs/analyze_shard_key_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const dbName = "testDb";
const collName = "collection0";
const nss = dbName + '.' + collName;
const kNumDocs = 100;
const kSampleRate = 100000;

function getCurrentOp(st) {
    const adminDB = st.s.getDB("admin");
    const mongosResult = adminDB
                             .aggregate([
                                 {$currentOp: {allUsers: true, localOps: true}},
                                 {$match: {desc: "query analyzer"}}
                             ])
                             .toArray();
    const mongodResult = assert
                             .commandWorked(st.rs0.getPrimary().adminCommand(
                                 {currentOp: true, desc: "query analyzer"}))
                             .inprog;
    return {"mongos": mongosResult, "mongod": mongodResult};
}

{
    const st = new ShardingTest({
        shards: 1,
        rs: {
            nodes: 2,
            setParameter: {
                queryAnalysisWriterIntervalSecs: 1,
            }
        },
        mongosOptions: {
            setParameter: {
                queryAnalysisSamplerConfigurationRefreshSecs: 1,
            }
        }
    });

    const mydb = st.s.getDB(dbName);
    const collection = mydb.getCollection(collName);

    //// Insert initial documents.

    const bulk = collection.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumDocs; i++) {
        bulk.insert({x: i, y: i});
    }
    assert.commandWorked(bulk.execute());

    //// Execute currentOp before any query sampling.

    let result = getCurrentOp(st);
    let mongosResult = result.mongos;
    let mongodResult = result.mongod;
    assert.eq(mongosResult.length, 0);
    assert.eq(mongodResult.length, 0);

    let expectedReadsCount = 0;
    let expectedWritesCount = 0;
    let expectedReadsBytes = 0;
    let expectedWritesBytes = 0;

    //// Start query analysis and send find queries.
    assert.commandWorked(
        st.s.adminCommand({configureQueryAnalyzer: nss, mode: "full", sampleRate: kSampleRate}));

    QuerySamplingUtil.waitForActiveSampling(st.s);

    for (let i = 0; i < kNumDocs; i++) {
        assert.commandWorked(mydb.runCommand({find: collName, filter: {x: i}}));
        ++expectedReadsCount;
        // The sample document size is determined empirically.
        expectedReadsBytes += 181;
    }

    result = getCurrentOp(st);
    mongosResult = result.mongos;
    mongodResult = result.mongod;

    assert.eq(mongosResult.length, 1);
    assert.eq(mongosResult[0].sampleRate, kSampleRate);
    assert.eq(mongosResult[0].sampledReadsCount, expectedReadsCount);
    assert.eq(mongosResult[0].sampledWritesCount, expectedWritesCount);

    assert.eq(mongodResult.length, 1);
    assert.eq(mongodResult[0].sampledReadsCount, expectedReadsCount);
    assert.eq(mongodResult[0].sampledReadsBytes, expectedReadsBytes);
    assert.eq(mongodResult[0].sampledWritesCount, expectedWritesCount);
    assert.eq(mongodResult[0].sampledWritesBytes, expectedWritesBytes);

    //// Send update queries.

    for (let i = 0; i < kNumDocs; ++i) {
        assert.commandWorked(
            mydb.runCommand({update: collName, updates: [{q: {x: i}, u: {updated: true}}]}));
        ++expectedWritesCount;
        // The sample document size is determined empirically.
        expectedWritesBytes += 327;
    }

    result = getCurrentOp(st);
    mongosResult = result.mongos;
    mongodResult = result.mongod;

    assert.eq(mongosResult.length, 1);
    assert.eq(mongosResult[0].sampleRate, kSampleRate);
    assert.eq(mongosResult[0].sampledReadsCount, expectedReadsCount);
    assert.eq(mongosResult[0].sampledWritesCount, expectedWritesCount);

    assert.eq(mongodResult.length, 1);
    assert.eq(mongodResult[0].sampledReadsCount, expectedReadsCount);
    assert.eq(mongodResult[0].sampledReadsBytes, expectedReadsBytes);
    assert.eq(mongodResult[0].sampledWritesCount, expectedWritesCount);
    assert.eq(mongodResult[0].sampledWritesBytes, expectedWritesBytes);

    //// Send findAndModify queries.

    for (let i = 0; i < kNumDocs; ++i) {
        const result = assert.commandWorked(mydb.runCommand(
            {findAndModify: collName, query: {updated: true}, update: {$set: {modified: 1}}}));
        ++expectedWritesCount;
        // The sample document size is determined empirically.
        expectedWritesBytes += 292;
    }

    result = getCurrentOp(st);
    mongosResult = result.mongos;
    mongodResult = result.mongod;

    assert.eq(mongosResult.length, 1);
    assert.eq(mongosResult[0].sampleRate, kSampleRate);
    assert.eq(mongosResult[0].sampledReadsCount, expectedReadsCount);
    assert.eq(mongosResult[0].sampledWritesCount, expectedWritesCount);

    assert.eq(mongodResult.length, 1);
    assert.eq(mongodResult[0].sampledReadsCount, expectedReadsCount);
    assert.eq(mongodResult[0].sampledReadsBytes, expectedReadsBytes);
    assert.eq(mongodResult[0].sampledWritesCount, expectedWritesCount);
    assert.eq(mongodResult[0].sampledWritesBytes, expectedWritesBytes);

    //// Send delete queries.

    for (let i = 0; i < kNumDocs; ++i) {
        assert.commandWorked(mydb.runCommand({delete: collName, deletes: [{q: {x: i}, limit: 1}]}));
        ++expectedWritesCount;
        // The sample document size is determined empirically.
        expectedWritesBytes += 303;
    }

    result = getCurrentOp(st);
    mongosResult = result.mongos;
    mongodResult = result.mongod;

    assert.eq(mongosResult.length, 1);
    assert.eq(mongosResult[0].sampleRate, kSampleRate);
    assert.eq(mongosResult[0].sampledReadsCount, expectedReadsCount);
    assert.eq(mongosResult[0].sampledWritesCount, expectedWritesCount);

    assert.eq(mongodResult.length, 1);
    assert.eq(mongodResult[0].sampledReadsCount, expectedReadsCount);
    assert.eq(mongodResult[0].sampledReadsBytes, expectedReadsBytes);
    assert.eq(mongodResult[0].sampledWritesCount, expectedWritesCount);
    assert.eq(mongodResult[0].sampledWritesBytes, expectedWritesBytes);

    st.stop();
}
})();
