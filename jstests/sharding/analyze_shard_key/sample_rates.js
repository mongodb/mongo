/**
 * Tests that query sampling respects the sample rate configured via the 'configureQueryAnalyzer'
 * command, and that the number of queries sampled by each mongos or shardsvr mongod is
 * proportional to the number of queries it executes.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

// Make the periodic jobs for refreshing sample rates and writing sampled queries and diffs have a
// period of 1 second to speed up the test.
const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const queryAnalysisWriterIntervalSecs = 1;

// Test both the sharded and unsharded case. Set up the following collections:
// - a sharded collection to be used for testing query sampling.
// - an unsharded collection to be used for testing query sampling.
// - an unsharded collection to be used as the local collection when testing sampling nested
//   aggregate queries against the two collections above.
const dbName = "testDb";
const collNameSampledUnsharded = "sampledCollUnsharded";
const collNameSampledSharded = "sampledCollSharded";
const collNameNotSampled = "notSampledColl";
const sampledNsSharded = dbName + "." + collNameSampledSharded;

// Make mongos0, mongos1 and shard0 (primary shard) act as samplers. Make mongos2 not refresh its
// sample rates or report its uptime to get test coverage for having an inactive sampler.
const st = new ShardingTest({
    mongos: {
        s0: {setParameter: {queryAnalysisSamplerConfigurationRefreshSecs}},
        s1: {setParameter: {queryAnalysisSamplerConfigurationRefreshSecs}},
        s2: {
            setParameter: {
                "failpoint.disableQueryAnalysisSampler": tojson({mode: "alwaysOn"}),
                "failpoint.disableShardingUptimeReporting": tojson({mode: "alwaysOn"})
            }
        }
    },
    shards: 3,
    rs: {
        nodes: 2,
        setParameter:
            {queryAnalysisSamplerConfigurationRefreshSecs, queryAnalysisWriterIntervalSecs}
    },
    configOptions: {
        setParameter: {queryAnalysisSamplerInActiveThresholdSecs: 3},
    }
});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.name);
const mongosDB = st.s.getDB(dbName);

// Set up the unsharded sampled collection.
assert.commandWorked(mongosDB.getCollection(collNameSampledUnsharded).insert([{x: 0}]));

// Set up the sharded sampled collection. Make it have three chunks:
// shard0: [MinKey, 0]
// shard1: [0, 1000]
// shard1: [1000, MaxKey]
assert.commandWorked(st.s.adminCommand({shardCollection: sampledNsSharded, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: sampledNsSharded, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({split: sampledNsSharded, middle: {x: 1000}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: sampledNsSharded, find: {x: 0}, to: st.shard1.shardName}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: sampledNsSharded, find: {x: 1000}, to: st.shard2.shardName}));

// Set up the non sampled collection. It needs to have at least one document. Otherwise, no nested
// aggregate queries would be issued.
assert.commandWorked(mongosDB.getCollection(collNameNotSampled).insert([{a: 0}]));

/**
 * Tries to run randomly generated find commands against the collection 'collName' in the database
 * 'dbName' at rate 'targetNumPerSec' for 'durationSecs'. Returns the actual rate.
 */
function runFindCmdsOnRepeat(mongosHost, dbName, collName, targetNumPerSec, durationSecs) {
    load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");
    const mongos = new Mongo(mongosHost);
    const db = mongos.getDB(dbName);
    const makeCmdObjFunc = () => {
        const xVal = AnalyzeShardKeyUtil.getRandInteger(1, 500);
        // Use a range filter half of the time to get test coverage for targeting more than one
        // shard in the sharded case.
        const filter = Math.random() > 0.5 ? {x: {$gte: xVal}} : {x: xVal};
        const collation = QuerySamplingUtil.generateRandomCollation();
        return {find: collName, filter, collation};
    };
    return QuerySamplingUtil.runCmdsOnRepeat(db, makeCmdObjFunc, targetNumPerSec, durationSecs);
}

/**
 * Tries to run randomly generated delete commands against the collection 'collName' in the database
 * 'dbName' at rate 'targetNumPerSec' for 'durationSecs'. Returns the actual rate.
 */
function runDeleteCmdsOnRepeat(mongosHost, dbName, collName, targetNumPerSec, durationSecs) {
    load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");
    const mongos = new Mongo(mongosHost);
    const db = mongos.getDB(dbName);
    const makeCmdObjFunc = () => {
        const xVal = AnalyzeShardKeyUtil.getRandInteger(1, 500);
        // Use a range filter half of the time to get test coverage for targeting more than one
        // shard in the sharded case.
        const filter = Math.random() > 0.5 ? {x: {$gte: xVal}} : {x: xVal};
        const collation = QuerySamplingUtil.generateRandomCollation();
        return {delete: collName, deletes: [{q: filter, collation, limit: 0}]};
    };
    return QuerySamplingUtil.runCmdsOnRepeat(db, makeCmdObjFunc, targetNumPerSec, durationSecs);
}

/**
 * Tries to run randomly generated aggregate commands with a $lookup stage against the collections
 * 'localCollName' and 'foreignCollName' in the database 'dbName' at rate 'targetNumPerSec' for
 * 'durationSecs'. Returns the actual rate.
 */
function runNestedAggregateCmdsOnRepeat(
    mongosHost, dbName, localCollName, foreignCollName, targetNumPerSec, durationSecs) {
    load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");
    const mongos = new Mongo(mongosHost);
    const db = mongos.getDB(dbName);
    const makeCmdObjFunc = () => {
        const xVal = AnalyzeShardKeyUtil.getRandInteger(1, 500);
        // Use a range filter half of the time to get test coverage for targeting more than one
        // shard in the sharded case.
        const filter = Math.random() > 0.5 ? {x: {$gte: xVal}} : {x: xVal};
        const collation = QuerySamplingUtil.generateRandomCollation();
        return {
            aggregate: localCollName,
            pipeline:
                [{$lookup: {from: foreignCollName, as: "joined", pipeline: [{$match: filter}]}}],
            collation,
            cursor: {},
            $readPreference: {mode: "primary"}
        };
    };
    return QuerySamplingUtil.runCmdsOnRepeat(db, makeCmdObjFunc, targetNumPerSec, durationSecs);
}

/**
 * Returns the number of sampled queries by command name along with the total number.
 */
function getSampleSize(st) {
    let sampleSize = {total: 0};

    st._rs.forEach((rs) => {
        const docs = rs.test.getPrimary().getCollection("config.sampledQueries").find().toArray();
        sampleSize.total += docs.length;

        docs.forEach(doc => {
            if (!sampleSize.hasOwnProperty(doc.cmdName)) {
                sampleSize[[doc.cmdName]] = 0;
            }
            sampleSize[[doc.cmdName]] += 1;
        });
    });

    return sampleSize;
}

/**
 * Asserts that the difference between 'actual' and 'expected' is less than 'maxPercentage' of
 * 'expected'.
 */
function assertDiffPercentage(actual, expected, maxPercentage) {
    const actualPercentage = Math.abs(actual - expected) * 100 / expected;
    assert.lt(actualPercentage,
              maxPercentage,
              tojson({actual, expected, maxPercentage, actualPercentage}));
}

/**
 * Tests that query sampling respects the configured sample rate and that the number of queries
 * sampled by each mongos or shardsvr mongod is proportional to the number of queries it executes.
 */
function testQuerySampling(dbName, collNameNotSampled, collNameSampled) {
    const sampledNs = dbName + "." + collNameSampled;
    const sampleRate = 5;
    const durationSecs = 90;

    assert.commandWorked(
        st.s.adminCommand({configureQueryAnalyzer: sampledNs, mode: "full", sampleRate}));
    sleep(queryAnalysisSamplerConfigurationRefreshSecs * 1000);

    // Define a thread for executing find commands via mongos0.
    const targetNumFindPerSec = 25;
    const findThread = new Thread(runFindCmdsOnRepeat,
                                  st.s0.host,
                                  dbName,
                                  collNameSampled,
                                  targetNumFindPerSec,
                                  durationSecs);

    // Define a thread for executing delete commands via mongos1.
    const targetNumDeletePerSec = 20;
    const deleteThread = new Thread(runDeleteCmdsOnRepeat,
                                    st.s1.host,
                                    dbName,
                                    collNameSampled,
                                    targetNumDeletePerSec,
                                    durationSecs);

    // Define a thread for executing aggregate commands via mongos2 (more specifically, shard0's
    // primary).
    const targetNumAggPerSec = 10;
    const aggThread = new Thread(runNestedAggregateCmdsOnRepeat,
                                 st.s2.host,
                                 dbName,
                                 collNameNotSampled,
                                 collNameSampled,
                                 targetNumAggPerSec,
                                 durationSecs);

    // Run the commands.
    findThread.start();
    deleteThread.start();
    aggThread.start();
    const actualNumFindPerSec = findThread.returnData();
    const actualNumDeletePerSec = deleteThread.returnData();
    const actualNumAggPerSec = aggThread.returnData();
    const actualTotalQueriesPerSec =
        actualNumFindPerSec + actualNumDeletePerSec + actualNumAggPerSec;

    assert.commandWorked(st.s.adminCommand({configureQueryAnalyzer: sampledNs, mode: "off"}));
    sleep(queryAnalysisWriterIntervalSecs * 1000);

    // Wait for all the queries to get written to disk.
    let sampleSize;
    let prevTotal = 0;
    assert.soon(() => {
        sampleSize = getSampleSize(st);
        if (sampleSize.total == 0 || sampleSize.total != prevTotal) {
            prevTotal = sampleSize.total;
            return false;
        }
        return true;
    });
    jsTest.log("Finished waiting for sampled queries: " + tojsononeline({sampleSize}));

    // Verify that the difference between the actual and expected number of samples is within the
    // expected threshold.
    const expectedTotalCount = durationSecs * sampleRate;
    const expectedFindCount = (actualNumFindPerSec / actualTotalQueriesPerSec) * expectedTotalCount;
    const expectedDeleteCount =
        (actualNumDeletePerSec / actualTotalQueriesPerSec) * expectedTotalCount;
    const expectedAggCount = (actualNumAggPerSec / actualTotalQueriesPerSec) * expectedTotalCount;
    jsTest.log("Checking that the number of sampled queries is within the threshold: " +
               tojsononeline(
                   {expectedTotalCount, expectedFindCount, expectedDeleteCount, expectedAggCount}));

    assertDiffPercentage(sampleSize.total, expectedTotalCount, 5 /* maxDiffPercentage */);
    assertDiffPercentage(sampleSize.find, expectedFindCount, 10 /* maxDiffPercentage */);
    assertDiffPercentage(sampleSize.delete, expectedDeleteCount, 10 /* maxDiffPercentage */);
    assertDiffPercentage(sampleSize.aggregate, expectedAggCount, 10 /* maxDiffPercentage */);

    QuerySamplingUtil.clearSampledQueryCollectionOnAllShards(st);
}

testQuerySampling(dbName, collNameNotSampled, collNameSampledSharded);
testQuerySampling(dbName, collNameNotSampled, collNameSampledUnsharded);

st.stop();
})();
