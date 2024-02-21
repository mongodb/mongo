/**
 * Ensure serverStatus reports the total time spent sampling the oplog for all storage engines that
 * support OplogTruncateMarkers.
 * @tags: [
 *   requires_persistence,
 * ]
 */
const kOplogDocs = 47500;
// kNumOplogSamples is derived from the number of oplog entries above.
// Formula is kRandomSamplesPerMarker * numRecords / estimatedRecordsPerMarker, where
// kRandomSamplesPerMarker = 10
// numRecords = kOplogDocs + some small number of bookkeeping records
// estimatedRecordsPerMarker = (16MB / average oplog record size), empirically about 28700 records.
// The number of samples is picked to NOT be divisible by kLoggingIntervalSeconds so we can
// safely miss a logging interval without failing; this can sometimes happen due to clock
// adjustment.
const kNumOplogSamples = 16;
const kOplogSampleReadDelay = 1;
const kLoggingIntervalSeconds = 3;

const testDB = "test";

// Force oplog sampling to occur on start up for small numbers of oplog inserts.
const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            "maxOplogTruncationPointsDuringStartup": 10,
            "oplogSamplingLogIntervalSeconds": kLoggingIntervalSeconds,
            // TODO SERVER-74250: Change to slowCollectionSamplingReads when 7.0 releases
            "failpoint.slowOplogSamplingReads":
                tojson({mode: "alwaysOn", data: {"delay": kOplogSampleReadDelay}}),
            logComponentVerbosity: tojson({storage: {verbosity: 2}}),
        }
    }
});
replSet.startSet();
replSet.initiate();

let coll = replSet.getPrimary().getDB(testDB).getCollection("testcoll");

// Make sure each insert is an oplog entry.
assert.commandWorked(
    replSet.getPrimary().adminCommand({setParameter: 1, internalInsertMaxBatchSize: 1}));
// Insert enough documents to force kNumOplogSamples to be taken on the following start up.
let docsRemaining = kOplogDocs;
let docsDone = 0;
while (docsRemaining) {
    let batchDocs = docsRemaining > 1000 ? 1000 : docsRemaining;
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < batchDocs; i++) {
        bulk.insert({m: 1 + i + docsDone});
    }
    assert.commandWorked(bulk.execute());
    docsRemaining -= batchDocs;
    docsDone += batchDocs;
}

// Restart replica set to load entries from the oplog for sampling.
replSet.stopSet(null /* signal */, true /* forRestart */);
replSet.startSet({restart: true});

assert.commandWorked(replSet.getPrimary().getDB(testDB).serverStatus());

// Err on the side of a smaller minExpectedLogs where fractional parts are concerned because
// kLoggingIntervalSeconds is not an exact interval. Rather, once interval seconds have elapsed
// since the last log message, a progress message will be logged after the current sample is
// completed.
const maxSamplesPerLog = Math.ceil(kLoggingIntervalSeconds / kOplogSampleReadDelay);
const minExpectedLogs = Math.floor(kNumOplogSamples / maxSamplesPerLog);

checkLog.containsWithAtLeastCount(
    replSet.getPrimary(), RegExp("(Collection|Oplog) sampling progress"), minExpectedLogs);
assert(checkLog.checkContainsOnce(replSet.getPrimary(),
                                  RegExp("(Collection|Oplog) sampling complete")));

replSet.stopSet();
