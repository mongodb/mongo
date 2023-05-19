/**
 * Tests that query sampling respects the sample rate configured via the 'configureQueryAnalyzer'
 * command, and that the number of queries sampled by each mongos or shardsvr mongod in a sharded
 * cluster is proportional to the number of queries it executes.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/libs/parallelTester.js");
load("jstests/sharding/analyze_shard_key/libs/sample_rates_common.js");

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
        setParameter: {
            queryAnalysisSamplerConfigurationRefreshSecs,
            queryAnalysisWriterIntervalSecs,
            logComponentVerbosity: tojson({sharding: 3})
        }
    },
    configOptions: {
        setParameter: {
            queryAnalysisSamplerInActiveThresholdSecs: 3,
            queryAnalysisSamplerConfigurationRefreshSecs,
            queryAnalysisWriterIntervalSecs,
            logComponentVerbosity: tojson({sharding: 3})
        },
    }
});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.name);
const mongosDB = st.s.getDB(dbName);

// Set up the unsharded sampled collection.
assert.commandWorked(mongosDB.getCollection(collNameSampledUnsharded).insert([{[fieldName]: 0}]));

// Set up the sharded sampled collection. Make it have three chunks:
// shard0: [MinKey, 0]
// shard1: [0, 1000]
// shard1: [1000, MaxKey]
assert.commandWorked(st.s.adminCommand({shardCollection: sampledNsSharded, key: {[fieldName]: 1}}));
assert.commandWorked(st.s.adminCommand({split: sampledNsSharded, middle: {[fieldName]: 0}}));
assert.commandWorked(st.s.adminCommand({split: sampledNsSharded, middle: {[fieldName]: 1000}}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: sampledNsSharded, find: {[fieldName]: 0}, to: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: sampledNsSharded, find: {[fieldName]: 1000}, to: st.shard2.shardName}));

// Set up the non sampled collection. It needs to have at least one document. Otherwise, no nested
// aggregate queries would be issued.
assert.commandWorked(mongosDB.getCollection(collNameNotSampled).insert([{a: 0}]));

/**
 * Returns the number of sampled queries by command name along with the total number.
 */
function getSampleSize() {
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
        sampleSize = getSampleSize();
        if (sampleSize.total == 0 || sampleSize.total != prevTotal) {
            prevTotal = sampleSize.total;
            return false;
        }
        return true;
    });
    jsTest.log("Finished waiting for sampled queries: " +
               tojsononeline({actualSampleSize: sampleSize}));

    // Verify that the difference between the actual and expected number of samples is within the
    // expected threshold.
    const expectedTotalCount = durationSecs * sampleRate;
    const expectedFindPercentage =
        AnalyzeShardKeyUtil.calculatePercentage(actualNumFindPerSec, actualTotalQueriesPerSec);
    const expectedDeletePercentage =
        AnalyzeShardKeyUtil.calculatePercentage(actualNumDeletePerSec, actualTotalQueriesPerSec);
    const expectedAggPercentage =
        AnalyzeShardKeyUtil.calculatePercentage(actualNumAggPerSec, actualTotalQueriesPerSec);
    jsTest.log("Checking that the number of sampled queries is within the threshold: " +
               tojsononeline({
                   expectedSampleSize: {
                       total: expectedTotalCount,
                       find: expectedFindPercentage * expectedTotalCount / 100,
                       delete: expectedDeletePercentage * expectedTotalCount / 100,
                       aggregate: expectedAggPercentage * expectedTotalCount / 100
                   }
               }));

    AnalyzeShardKeyUtil.assertDiffPercentage(
        sampleSize.total, expectedTotalCount, 10 /* maxDiffPercentage */);
    const actualFindPercentage =
        AnalyzeShardKeyUtil.calculatePercentage(sampleSize.find, sampleSize.total);
    assertDiffWindow(actualFindPercentage, expectedFindPercentage, 5 /* maxDiff */);
    const actualDeletePercentage =
        AnalyzeShardKeyUtil.calculatePercentage(sampleSize.delete, sampleSize.total);
    assertDiffWindow(actualDeletePercentage, expectedDeletePercentage, 5 /* maxDiff */);
    const actualAggPercentage =
        AnalyzeShardKeyUtil.calculatePercentage(sampleSize.aggregate, sampleSize.total);
    assertDiffWindow(actualAggPercentage, expectedAggPercentage, 5 /* maxDiff */);

    QuerySamplingUtil.clearSampledQueryCollectionOnAllShards(st);
}

testQuerySampling(dbName, collNameNotSampled, collNameSampledSharded);
testQuerySampling(dbName, collNameNotSampled, collNameSampledUnsharded);

st.stop();
})();
