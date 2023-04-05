/**
 * Tests that query sampling respects the sample rate configured via the 'configureQueryAnalyzer'
 * command, and that the number of queries sampled by each mongod in a standalone replica set is
 * proportional to the number of queries it executes.
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

// Set up the following collections:
// - a collection to be used for testing query sampling.
// - a collection to be used as the local collection when testing sampling nested aggregate queries
//   against the collection above.
const dbName = "testDb";
const collNameSampled = "sampledColl";
const collNameNotSampled = "notSampledColl";

const rst = new ReplSetTest({
    nodes: 3,
    nodeOptions: {
        setParameter: {
            queryAnalysisSamplerConfigurationRefreshSecs,
            queryAnalysisWriterIntervalSecs,
            logComponentVerbosity: tojson({sharding: 2})
        },
    }
});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const secondaries = rst.getSecondaries();

const db = primary.getDB(dbName);

// Set up the sampled collection.
assert.commandWorked(db.getCollection(collNameSampled).insert([{x: 0}]));

// Set up the non sampled collection. It needs to have at least one document. Otherwise, no nested
// aggregate queries would be issued.
assert.commandWorked(db.getCollection(collNameNotSampled).insert([{a: 0}]));

/**
 * Returns the number of sampled queries by command name along with the total number.
 */
function getSampleSize() {
    let sampleSize = {total: 0};

    const docs = primary.getCollection("config.sampledQueries").find().toArray();
    sampleSize.total += docs.length;

    docs.forEach(doc => {
        if (!sampleSize.hasOwnProperty(doc.cmdName)) {
            sampleSize[[doc.cmdName]] = 0;
        }
        sampleSize[[doc.cmdName]] += 1;
    });

    return sampleSize;
}

/**
 * Tests that query sampling respects the configured sample rate and that the number of queries
 * sampled by each mongod is proportional to the number of queries it executes.
 */
function testQuerySampling(dbName, collNameNotSampled, collNameSampled) {
    const sampledNs = dbName + "." + collNameSampled;
    const sampleRate = 5;
    const durationSecs = 90;

    assert.commandWorked(
        primary.adminCommand({configureQueryAnalyzer: sampledNs, mode: "full", sampleRate}));
    sleep(queryAnalysisSamplerConfigurationRefreshSecs * 1000);

    // Define a thread for executing find commands via one of the secondaries.
    const targetNumFindPerSec = 100;
    const findThread = new Thread(runFindCmdsOnRepeat,
                                  secondaries[0].host,
                                  dbName,
                                  collNameSampled,
                                  targetNumFindPerSec,
                                  durationSecs);

    // Define a thread for executing delete commands via the primary.
    const targetNumDeletePerSec = 80;
    const deleteThread = new Thread(runDeleteCmdsOnRepeat,
                                    primary.host,
                                    dbName,
                                    collNameSampled,
                                    targetNumDeletePerSec,
                                    durationSecs);

    // Define a thread for executing aggregate commands via the other secondary.
    const targetNumAggPerSec = 40;
    const aggThread = new Thread(runNestedAggregateCmdsOnRepeat,
                                 secondaries[1].host,
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
    jsTest.log("actual rate " +
               tojson({actualNumFindPerSec, actualNumDeletePerSec, actualNumAggPerSec}));
    const actualTotalQueriesPerSec =
        actualNumFindPerSec + actualNumDeletePerSec + actualNumAggPerSec;

    assert.commandWorked(primary.adminCommand({configureQueryAnalyzer: sampledNs, mode: "off"}));
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

    AnalyzeShardKeyUtil.assertDiffPercentage(
        sampleSize.total, expectedTotalCount, 10 /* maxDiffPercentage */);
    AnalyzeShardKeyUtil.assertDiffPercentage(
        sampleSize.find, expectedFindCount, 15 /* maxDiffPercentage */);
    AnalyzeShardKeyUtil.assertDiffPercentage(
        sampleSize.delete, expectedDeleteCount, 15 /* maxDiffPercentage */);
    AnalyzeShardKeyUtil.assertDiffPercentage(
        sampleSize.aggregate, expectedAggCount, 15 /* maxDiffPercentage */);

    QuerySamplingUtil.clearSampledQueryCollection(primary);
}

testQuerySampling(dbName, collNameNotSampled, collNameSampled);

rst.stopSet();
})();
