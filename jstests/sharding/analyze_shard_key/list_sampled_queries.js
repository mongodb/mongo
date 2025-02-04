/**
 * Tests that listSampledQueries correctly returns sampled queries for both sharded clusters and
 * replica sets.
 *
 * @tags: [
 *   requires_fcv_70,
 *   requires_fcv_80,
 *   # TODO (SERVER-85629): Re-enable this test once redness is resolved in multiversion suites.
 *   DISABLED_TEMPORARILY_DUE_TO_FCV_UPGRADE
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";
import {QuerySamplingUtil} from "jstests/sharding/analyze_shard_key/libs/query_sampling_util.js";

const samplesPerSecond = 10000;

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
        assert.commandWorked(
            conn.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
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

    jsTest.log(
        "Test running a $listSampledQueries aggregate command while there are no sampled queries");
    let actualSamples = adminDb.aggregate([{$listSampledQueries: {}}]).toArray();
    assert.eq(actualSamples.length, 0);

    conn.adminCommand({configureQueryAnalyzer: ns0, mode: "full", samplesPerSecond});
    conn.adminCommand({configureQueryAnalyzer: ns1, mode: "full", samplesPerSecond});
    QuerySamplingUtil.waitForActiveSampling(ns0, collUuid0, {rst, st});
    QuerySamplingUtil.waitForActiveSampling(ns1, collUuid1, {rst, st});

    let expectedSamples = [];
    // Use this to identify expected samples later.
    let sampleNum = -1;
    const getSampleNum = (sample) => {
        switch (sample.cmdName) {
            case "aggregate":
            case "count":
            case "distinct":
            case "find":
                return sample.cmd.filter.sampleNum;
            case "update":
            case "delete":
            case "findAndModify":
            case "bulkWrite":
                return sample.cmd.let.sampleNum;
            default:
                throw Error("Unexpected command name");
        }
    };

    let numSamplesColl0 = 0;
    let numSamplesColl1 = 0;

    // Create read samples on collection0.
    const aggregateFilter = {x: 1, sampleNum: ++sampleNum};
    assert.commandWorked(testDb.runCommand(
        {aggregate: collName0, pipeline: [{$match: aggregateFilter}], cursor: {}}));
    expectedSamples[sampleNum] = {
        ns: ns0,
        collectionUuid: collUuid0,
        cmdName: "aggregate",
        cmd: {filter: aggregateFilter, collation: {locale: "simple"}}
    };
    numSamplesColl0++;

    const countFilter = {x: -1, sampleNum: ++sampleNum};
    assert.commandWorked(testDb.runCommand({count: collName0, query: countFilter}));
    expectedSamples[sampleNum] =
        {ns: ns0, collectionUuid: collUuid0, cmdName: "count", cmd: {filter: countFilter}};
    numSamplesColl0++;

    const distinctFilter = {x: 2, sampleNum: ++sampleNum};
    assert.commandWorked(testDb.runCommand({distinct: collName0, key: "x", query: distinctFilter}));
    expectedSamples[sampleNum] =
        {ns: ns0, collectionUuid: collUuid0, cmdName: "distinct", cmd: {filter: distinctFilter}};
    numSamplesColl0++;

    const findFilter = {x: -3, sampleNum: ++sampleNum};
    assert.commandWorked(testDb.runCommand({find: collName0, filter: findFilter, collation: {}}));
    expectedSamples[sampleNum] = {
        ns: ns0,
        collectionUuid: collUuid0,
        cmdName: "find",
        cmd: {filter: findFilter, collation: {}}
    };
    numSamplesColl0++;

    // Create write samples on collection1.
    const updateCmdObj = {
        update: collName1,
        updates: [{q: {x: 4}, u: [{$set: {y: 1}}], multi: false}],
        let : {sampleNum: ++sampleNum}
    };
    assert.commandWorked(testDb.runCommand(updateCmdObj));
    expectedSamples[sampleNum] = {
        ns: ns1,
        collectionUuid: collUuid1,
        cmdName: "update",
        cmd: Object.assign({}, updateCmdObj, {$db: dbName})
    };
    numSamplesColl1++;

    const findAndModifyCmdObj = {
        findAndModify: collName1,
        query: {x: 5},
        sort: {x: 1},
        update: {$set: {z: 1}},
        let : {sampleNum: ++sampleNum}
    };
    assert.commandWorked(testDb.runCommand(findAndModifyCmdObj));
    expectedSamples[sampleNum] = {
        ns: ns1,
        collectionUuid: collUuid1,
        cmdName: "findAndModify",
        cmd: Object.assign({}, findAndModifyCmdObj, {$db: dbName})
    };
    numSamplesColl1++;

    const deleteCmdObj = {
        delete: collName1,
        deletes: [{q: {x: -6}, limit: 1}],
        let : {sampleNum: ++sampleNum}
    };
    assert.commandWorked(testDb.runCommand(deleteCmdObj));
    expectedSamples[sampleNum] = {
        ns: ns1,
        collectionUuid: collUuid1,
        cmdName: "delete",
        cmd: Object.assign({}, deleteCmdObj, {$db: dbName})
    };
    numSamplesColl1++;

    // TODO (SERVER-67711): Remove feature flag for bulkWrite command.
    if (FeatureFlagUtil.isPresentAndEnabled(testDb, "BulkWriteCommand")) {
        // The bulkWrite feature is not FCV gated and is available (but incomplete) in server
        // version older than 7.3. In order to not run this portion of the test in multiversion
        // suites, we check the FCV version here and only do this test against cluster with FCV 7.3
        // or above (with binary version 7.3 or above implied).
        const getFCVDoc = () => {
            if (st) {
                return assert.commandWorked(
                    st.shard0.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));
            } else {
                return assert.commandWorked(
                    testDb.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));
            }
        };
        if (MongoRunner.compareBinVersions(getFCVDoc().featureCompatibilityVersion.version,
                                           '7.3') >= 0) {
            jsTestLog("Test bulkWrite command for sampling");
            const bulkWriteCmdObj = {
                bulkWrite: 1,
                ops: [{update: 0, filter: {x: 7}, updateMods: {$set: {y: 7}}}],
                nsInfo: [{ns: ns1}],
                let : {sampleNum: ++sampleNum}
            };
            assert.commandWorked(testDb.adminCommand(bulkWriteCmdObj));
            expectedSamples[sampleNum] = {
                ns: ns1,
                collectionUuid: collUuid1,
                cmdName: "bulkWrite",
                cmd: Object.assign({}, bulkWriteCmdObj, {$db: "admin"})
            };
            numSamplesColl1++;
        }
    }

    jsTest.log("Test running a $listSampledQueries aggregate command that doesn't involve " +
               "getMore commands");
    // Verify samples on both collections.
    assert.soon(() => {
        actualSamples = adminDb.aggregate([{$listSampledQueries: {}}, {$sort: {ns: 1}}]).toArray();
        return actualSamples.length >= (numSamplesColl0 + numSamplesColl1);
    });
    assert.eq(actualSamples.length, numSamplesColl0 + numSamplesColl1);
    actualSamples.forEach((sample) => {
        AnalyzeShardKeyUtil.validateSampledQueryDocument(sample);
        QuerySamplingUtil.assertSubObject(sample, expectedSamples[getSampleNum(sample)]);
    });

    // Verify that listing for collection0 returns only collection0 samples.
    assert.soon(() => {
        actualSamples = adminDb.aggregate([{$listSampledQueries: {namespace: ns0}}]).toArray();
        return actualSamples.length >= numSamplesColl0;
    });
    assert.eq(actualSamples.length, numSamplesColl0);
    actualSamples.forEach((sample) => {
        AnalyzeShardKeyUtil.validateSampledQueryDocument(sample);
        QuerySamplingUtil.assertSubObject(sample, expectedSamples[getSampleNum(sample)]);
    });

    // Verify that listing for collection1 returns only collection1 samples.
    assert.soon(() => {
        actualSamples = adminDb.aggregate([{$listSampledQueries: {namespace: ns1}}]).toArray();
        return actualSamples.length >= numSamplesColl1;
    });
    assert.eq(actualSamples.length, numSamplesColl1);
    actualSamples.forEach((sample) => {
        AnalyzeShardKeyUtil.validateSampledQueryDocument(sample);
        QuerySamplingUtil.assertSubObject(sample, expectedSamples[getSampleNum(sample)]);
    });

    jsTest.log("Test running a $listSampledQueries aggregate command that involves getMore " +
               "commands");
    // Make the number of sampled queries larger than the batch size so that getMore commands are
    // required when $listSampledQueries is run.
    const batchSize = 101;
    for (let i = 0; i < 250; i++) {
        const sign = (i % 2 == 0) ? 1 : -1;
        const findFilter = {x: sign * 7, sampleNum: ++sampleNum};
        assert.commandWorked(
            testDb.runCommand({find: collName1, filter: findFilter, collation: {}}));
        expectedSamples[sampleNum] = {
            ns: ns1,
            collectionUuid: collUuid1,
            cmdName: "find",
            cmd: {filter: findFilter, collation: {}}
        };
        numSamplesColl1++;
    }
    assert.soon(() => {
        actualSamples = adminDb.aggregate([{$listSampledQueries: {}}], {batchSize}).toArray();
        return actualSamples.length >= expectedSamples.length;
    });
    assert.eq(actualSamples.length, expectedSamples.length);
    actualSamples.forEach((sample) => {
        AnalyzeShardKeyUtil.validateSampledQueryDocument(sample);
        QuerySamplingUtil.assertSubObject(sample, expectedSamples[getSampleNum(sample)]);
    });

    jsTest.log("Test that running on a database other than \"admin\" results in error");
    assert.commandFailedWithCode(
        testDb.runCommand({aggregate: 1, pipeline: [{$listSampledQueries: {}}], cursor: {}}),
        ErrorCodes.InvalidNamespace);

    jsTest.log("Test that running with a namespace that contains a null byte results in error");
    assert.commandFailedWithCode(adminDb.runCommand({
        aggregate: 1,
        pipeline: [{$listSampledQueries: {namespace: "invalid_\x00_ns"}}],
        cursor: {}
    }),
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

if (jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove tests below
    quit();
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
