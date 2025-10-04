/**
 * Tests that query sampling respects the sample rate configured via the 'configureQueryAnalyzer'
 * command, and that the number of queries sampled by each mongod in a standalone replica set is
 * proportional to the number of queries it executes.
 *
 * @tags: [
 *   requires_fcv_70,
 *    # On slow machines, the number of sampled queries could fall outside the expected range.
 *    incompatible_aubsan,
 *    tsan_incompatible,
 *    incompatible_ppc,
 * ]
 */
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {AnalyzeShardKeyUtil} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";
import {QuerySamplingUtil} from "jstests/sharding/analyze_shard_key/libs/query_sampling_util.js";
import {
    assertDiffWindow,
    runDeleteCmdsOnRepeat,
    runFindCmdsOnRepeat,
    runNestedAggregateCmdsOnRepeat,
} from "jstests/sharding/analyze_shard_key/libs/sample_rates_common.js";
import {isSlowBuild} from "jstests/sharding/libs/sharding_util.js";

if (jsTestOptions().useAutoBootstrapProcedure) {
    // TODO: SERVER-80318 Delete test
    quit();
}

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
            logComponentVerbosity: tojson({sharding: 3}),
        },
    },
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

    docs.forEach((doc) => {
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
    const samplesPerSecond = 5;
    const durationSecs = 90;

    assert.commandWorked(primary.adminCommand({configureQueryAnalyzer: sampledNs, mode: "full", samplesPerSecond}));
    sleep(queryAnalysisSamplerConfigurationRefreshSecs * 1000);

    // Define a thread for executing find commands via one of the secondaries.
    const targetNumFindPerSec = 100;
    const findThread = new Thread(
        runFindCmdsOnRepeat,
        secondaries[0].host,
        dbName,
        collNameSampled,
        targetNumFindPerSec,
        durationSecs,
    );

    // Define a thread for executing delete commands via the primary.
    const targetNumDeletePerSec = 80;
    const deleteThread = new Thread(
        runDeleteCmdsOnRepeat,
        primary.host,
        dbName,
        collNameSampled,
        targetNumDeletePerSec,
        durationSecs,
    );

    // Define a thread for executing aggregate commands via the other secondary.
    const targetNumAggPerSec = 40;
    const aggThread = new Thread(
        runNestedAggregateCmdsOnRepeat,
        secondaries[1].host,
        dbName,
        collNameNotSampled,
        collNameSampled,
        targetNumAggPerSec,
        durationSecs,
    );

    // Run the commands.
    findThread.start();
    deleteThread.start();
    aggThread.start();
    const actualNumFindPerSec = findThread.returnData();
    const actualNumDeletePerSec = deleteThread.returnData();
    const actualNumAggPerSec = aggThread.returnData();
    jsTest.log("actual rate " + tojson({actualNumFindPerSec, actualNumDeletePerSec, actualNumAggPerSec}));
    const actualTotalQueriesPerSec = actualNumFindPerSec + actualNumDeletePerSec + actualNumAggPerSec;

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
    jsTest.log("Finished waiting for sampled queries: " + tojsononeline({actualSampleSize: sampleSize}));

    const deleteField = TestData.runningWithBulkWriteOverride ? "bulkWrite" : "delete";

    // Verify that the difference between the actual and expected number of samples is within the
    // expected threshold.
    const expectedTotalCount = durationSecs * samplesPerSecond;
    const expectedFindPercentage = AnalyzeShardKeyUtil.calculatePercentage(
        actualNumFindPerSec,
        actualTotalQueriesPerSec,
    );
    const expectedDeletePercentage = AnalyzeShardKeyUtil.calculatePercentage(
        actualNumDeletePerSec,
        actualTotalQueriesPerSec,
    );
    const expectedAggPercentage = AnalyzeShardKeyUtil.calculatePercentage(actualNumAggPerSec, actualTotalQueriesPerSec);

    const slowBuild = isSlowBuild(primary);
    jsTest.log(
        "Checking that the number of sampled queries is within the threshold: " +
            tojsononeline({
                expectedSampleSize: {
                    total: expectedTotalCount,
                    find: (expectedFindPercentage * expectedTotalCount) / 100,
                    [deleteField]: (expectedDeletePercentage * expectedTotalCount) / 100,
                    aggregate: (expectedAggPercentage * expectedTotalCount) / 100,
                },
                isSlowBuild: slowBuild,
            }),
    );

    // The maximum percentage difference between the actual and expected total number of samples.
    const maxTotalSampleDiffPercentage = slowBuild ? 50 : 25;
    // The maximum difference between the actual and expected percentage of samples for each
    // command.
    const maxCommandPercentageDiff = slowBuild ? 15 : 10;

    AnalyzeShardKeyUtil.assertDiffPercentage(sampleSize.total, expectedTotalCount, maxTotalSampleDiffPercentage);
    const actualFindPercentage = AnalyzeShardKeyUtil.calculatePercentage(sampleSize.find, sampleSize.total);
    assertDiffWindow(actualFindPercentage, expectedFindPercentage, maxCommandPercentageDiff);
    const actualDeletePercentage = AnalyzeShardKeyUtil.calculatePercentage(sampleSize[deleteField], sampleSize.total);
    assertDiffWindow(actualDeletePercentage, expectedDeletePercentage, maxCommandPercentageDiff);
    const actualAggPercentage = AnalyzeShardKeyUtil.calculatePercentage(sampleSize.aggregate, sampleSize.total);
    assertDiffWindow(actualAggPercentage, expectedAggPercentage, maxCommandPercentageDiff);

    QuerySamplingUtil.clearSampledQueryCollection(primary);
}

testQuerySampling(dbName, collNameNotSampled, collNameSampled);

rst.stopSet();
