/**
 * Tests that bulkWrite command with multiple namespaces respects the sample rate configured via the
 * 'configureQueryAnalyzer' command.
 *
 * @tags: [
 *   requires_fcv_80,
 *   # Slow Windows machines cause this test to be flakey. Further reducing the number of samples we
 *   # take would make the test less useful on Linux variants so we just don't run on Windows.
 *   incompatible_with_windows_tls,
 *    # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 * ]
 */
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";
import {
    fieldName,
    runBulkWriteDeleteCmdsOnRepeat
} from "jstests/sharding/analyze_shard_key/libs/sample_rates_common.js";

// Make the periodic jobs for refreshing sample rates and writing sampled queries and diffs have a
// period of 1 second to speed up the test.
const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const queryAnalysisWriterIntervalSecs = 1;

// Set up the following collections:
// - a collection to be used for testing query sampling.
// - a collection with sampling disabled.
const dbName = "testDb";
const collNameSampled = "sampledColl";
const collNameNotSampled = "notSampledColl";
const sampledNs = dbName + "." + collNameSampled;
const notSampledNs = dbName + "." + collNameNotSampled;

/**
 * Returns the number of sampled queries by command name along with the total number.
 */
function getSampleSize(conn) {
    let sampleSize = {total: 0};

    const docs = conn.getCollection("config.sampledQueries").find().toArray();
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
 * Tests that the bulkWrite command respects the configured sample rate, even when the command
 * involves operations against multiple namespaces and only one of the namespaces has sampling
 * enabled.
 */
function testQuerySampling(conn, sampleConn, dbName, collNameNotSampled, collNameSampled) {
    const samplesPerSecond = 3;
    const durationSecs = 30;

    assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: sampledNs, mode: "full", samplesPerSecond}));
    sleep(queryAnalysisSamplerConfigurationRefreshSecs * 1000);

    const targetNumBulkWriteDeletePerSec = 15;
    const bulkWriteThread = new Thread(runBulkWriteDeleteCmdsOnRepeat,
                                       conn.host,
                                       dbName,
                                       collNameSampled,
                                       collNameNotSampled,
                                       targetNumBulkWriteDeletePerSec,
                                       durationSecs);

    bulkWriteThread.start();
    const actualNumBulkWritePerSec = bulkWriteThread.returnData();
    jsTest.log("actual rate " + actualNumBulkWritePerSec);

    assert.commandWorked(conn.adminCommand({configureQueryAnalyzer: sampledNs, mode: "off"}));
    sleep(queryAnalysisWriterIntervalSecs * 1000);

    // Wait for all the queries to get written to disk.
    let sampleSize;
    let prevTotal = 0;
    assert.soon(() => {
        sampleSize = getSampleSize(sampleConn);
        if (sampleSize.total == 0 || sampleSize.total != prevTotal) {
            prevTotal = sampleSize.total;
            return false;
        }
        return true;
    });

    jsTest.log("Finished waiting for sampled queries: " +
               tojsononeline({actualSampleSize: sampleSize}));

    assert.eq(sampleSize.total, sampleSize.bulkWrite);

    // Verify that the difference between the actual and expected number of samples is within the
    // expected threshold.
    const expectedTotalCount = durationSecs * samplesPerSecond;
    AnalyzeShardKeyUtil.assertDiffPercentage(
        sampleSize.total, expectedTotalCount, 10 /* maxDiffPercentage */);

    // Verify that no operation against the notSampledNs was sampled.
    const queriesNotSampledColl =
        conn.getCollection("config.sampledQueries").find({ns: notSampledNs}).toArray();
    assert.eq(queriesNotSampledColl.length, 0, () => tojson(queriesNotSampledColl));
}

(function testReplSet() {
    if (jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Delete test
        return;
    }

    jsTestLog("Running test against a replica set");
    const rst = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            setParameter: {
                queryAnalysisSamplerConfigurationRefreshSecs,
                queryAnalysisWriterIntervalSecs,
                logComponentVerbosity: tojson({sharding: 3})
            },
        }
    });
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    // Set up the collections.
    const db = primary.getDB(dbName);
    assert.commandWorked(db.getCollection(collNameSampled).insert([{[fieldName]: 0}]));
    assert.commandWorked(db.getCollection(collNameNotSampled).insert([{[fieldName]: 0}]));
    try {
        testQuerySampling(primary, primary, dbName, collNameNotSampled, collNameSampled);
    } finally {
        rst.stopSet();
    }
})();

(function testSharding() {
    jsTestLog("Running test against a sharded cluster");
    const st = new ShardingTest({
        mongos: {
            s0: {setParameter: {queryAnalysisSamplerConfigurationRefreshSecs}},
        },
        shards: 1,
        rs0: {
            nodes: 1,
            setParameter: {
                queryAnalysisSamplerConfigurationRefreshSecs,
                queryAnalysisWriterIntervalSecs,
                logComponentVerbosity: tojson({sharding: 3})
            }
        },
        config: {nodes: 1},
    });
    const mongosDB = st.s.getDB(dbName);
    // Set up the collections.
    assert.commandWorked(mongosDB.getCollection(collNameSampled).insert([{[fieldName]: 0}]));
    assert.commandWorked(mongosDB.getCollection(collNameNotSampled).insert([{[fieldName]: 0}]));
    try {
        testQuerySampling(st.s, st.rs0.getPrimary(), dbName, collNameNotSampled, collNameSampled);
    } finally {
        st.stop();
    }
})();
