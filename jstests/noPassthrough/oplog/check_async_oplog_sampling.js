/**
 * Checks that the oplog sampling and initial marker creation does not block startup and can
 * successfully complete post-startup. This is done by purposefully hanging the oplog cap maintainer
 * thread to force it to finish marker creation until after startup has finished.
 *     - readonly
 *     - repair
 *     - recoverFromOplogAsStandalone
 *
 * @tags: [requires_replication, requires_persistence]
 */
import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const oplogSizeMB = 1; // Replica set oplog size in MB
const oplogSizeBytes = oplogSizeMB * 1024 * 1024;
const minBytesPerMarker = 100 * 1024; // Minimum size per truncate marker (100 KB)
const smallDocSize = 1 * 1024; // Small doc size (1 KB)
const smallDoc = {
    payload: "a".repeat(smallDocSize),
}; // Small data for insertion

const kMinSampleRatioForRandCursor = 20; // Minimum sample ratio for random cursors
const kRandomSamplesPerMarker = 10; // Random samples drawn per marker
const estimatedMarkers = Math.ceil(oplogSizeBytes / minBytesPerMarker);

/**
 * Adds documents to grow the oplog beyond the configured maximum size.
 */
function growOplog(replSet) {
    const primary = replSet.getPrimary();
    const oplog = primary.getDB("local").oplog.rs;
    const coll = primary.getDB("growOplogPastMaxSize").testCollection;

    const initialOplogSize = oplog.dataSize();
    const initialRecordsCount = oplog.count();

    assert.lte(initialOplogSize, oplogSizeBytes, "Initial oplog size exceeds expected maximum");
    jsTestLog(`Initial oplog size: ${initialOplogSize} bytes, records: ${initialRecordsCount}`);

    jsTestLog(`RequiredNumRecords: ${requiredNumRecords}`);
    let insertedDocs = 0; // Track the number of inserted documents
    while (oplog.dataSize() <= 2 * oplogSizeBytes || insertedDocs < requiredNumRecords) {
        assert.commandWorked(coll.insert(smallDoc, {writeConcern: {w: 1}}));
        insertedDocs++;
        jsTestLog(`InsertedDocsNum: ${insertedDocs}`);
    }

    const finalOplogSize = oplog.dataSize();
    const finalRecordsCount = oplog.count();

    jsTestLog(`Final oplog size: ${finalOplogSize} bytes, records: ${finalRecordsCount}`);
    assert.gt(finalOplogSize, oplogSizeBytes, "Failed to grow oplog beyond the maximum size");
    assert.gte(finalRecordsCount, requiredNumRecords, "Failed to meet required number of records for sampling");
}

// Minimum number of records required to trigger sampling.
const requiredNumRecords = kMinSampleRatioForRandCursor * kRandomSamplesPerMarker * estimatedMarkers;

// Initialize replica set with a small oplog size
const rst = new ReplSetTest({
    oplogSize: oplogSizeMB,
    nodes: 1,
    nodeOptions: {
        setParameter: {
            logComponentVerbosity: tojson({storage: 1}),
            minOplogTruncationPoints: 2,
            "failpoint.hangOplogCapMaintainerThread": tojson({mode: "alwaysOn"}),
        },
    },
});

rst.startSet();
rst.initiate();
jsTestLog("Replica set initialized and starting single-node setup");

const primary = rst.getPrimary();

jsTestLog("Growing oplog to exceed its configured maximum size");
growOplog(rst);

// Verify logs related to oplog maintenance and marker creation
rst.stopSet(null, true); // Stop replica set for restart

jsTestLog("Restarting replica set");

rst.startSet(null, true);
const restartedPrimary = rst.getPrimary();

checkLog.containsJson(restartedPrimary, 4615611); // Log ID: Starting up MongoDB

checkLog.containsJson(restartedPrimary, 5295000); // Log ID: OplogCapMaintainerThread started

// Verify that the oplog cap maintainer thread is paused.
assert.commandWorked(
    restartedPrimary.adminCommand({
        waitForFailPoint: "hangOplogCapMaintainerThread",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

// Verify that that our truncate marker creation can end post startup.
checkLog.containsJson(restartedPrimary, 8423403); // Log ID: Start up finished

// Let the oplog cap maintainer thread finish creating truncate markers
assert.commandWorked(restartedPrimary.adminCommand({configureFailPoint: "hangOplogCapMaintainerThread", mode: "off"}));

// Note that log ID 22382 should appear after log ID 8323403
checkLog.containsJson(restartedPrimary, 22382); // Log ID: Oplog truncate markers calculated

jsTestLog("Test complete. Stopping replica set");
rst.stopSet();
