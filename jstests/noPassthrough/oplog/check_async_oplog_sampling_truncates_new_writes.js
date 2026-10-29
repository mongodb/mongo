/**
 * Checks that when async oplog sampling enabled, any data that is written in parallel with initial
 * sampling eventually gets truncated. This test also checks that the oplog sampling and initial
 * marker creation does not block startup and can successfully complete post-startup.
 *
 * @tags: [requires_replication, requires_persistence]
 */
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {skipTestIfSizeBasedOplogTruncationDisabled} from "jstests/libs/oplog_truncation_util.js";

// Constants for replica set and test configuration
const oplogSizeMB = 1; // Small oplog size in MB
const longString = "a".repeat(450 * 1024); // Large document size (~500KB)
let nextId = 0; // Tracks the next `_id` for inserts

// setParameter args to reuse across startups
const setParameter = {
    logComponentVerbosity: tojson({storage: 1}),
    minOplogTruncationPoints: 2,
    internalQueryExecYieldPeriodMS: 86400000, // Disable yielding

    // Speed up the wakeup time of the cap maintainer thread
    oplogTruncationCheckPeriodSeconds: 1,
};

// Initialize a single-node replica set with a small oplog size
const rst = new ReplSetTest({
    oplogSize: oplogSizeMB,
    nodes: 1,
    nodeOptions: {
        // Set the syncdelay to 1s to speed up checkpointing (note: this arg has no effect in DSC)
        syncdelay: 1,
        setParameter: setParameter,
    },
});
rst.startSet({
    oplogMinRetentionHours: 0.000001, // disable time-based retention
});
rst.initiate();

// This test relies on marker-based oplog truncation, which may be disabled in disagg.
// TODO(SERVER-125068) remove this once this feature flag is deleted
skipTestIfSizeBasedOplogTruncationDisabled(rst.getPrimary(), () => rst.stopSet());

// Insert initial documents
jsTest.log.info("Inserting initial set of documents into the collection.");
for (let i = 0; i < 10; i++) {
    rst.getPrimary().getDB("test").getCollection("markers").insert({_id: nextId++});
}

// Stop and restart the replica set
rst.stopSet(null, true);
jsTest.log.info("Replica set stopped for restart.");
clearRawMongoProgramOutput();

rst.startSet({
    restart: true,
    setParameter: {
        "failpoint.hangDuringOplogSampling": tojson({mode: "alwaysOn"}),
        ...setParameter,
    },
    oplogMinRetentionHours: 0.000001, // disable time-based retention
}); // Restart replica set

const restartedPrimary = rst.getPrimary();
const restartedPrimaryOplog = restartedPrimary.getDB("local").getCollection("oplog.rs");
jsTest.log.info("Replica set restarted.");

// Verify that the oplog cap maintainer thread is paused.
assert.commandWorked(
    restartedPrimary.adminCommand({
        waitForFailPoint: "hangDuringOplogSampling",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

// Verify truncate marker creation resumes post-startup
checkLog.containsJson(restartedPrimary, 8423403); // Log ID for startup finished

// Test inserts while truncate marker creation process is paused
jsTest.log.info("Testing oplog truncation logic with new inserts...");
const coll = restartedPrimary.getDB("test").markers;
const largeDocIDs = [nextId++, nextId++];

// Insert large documents
const firstInsertTimestamp = assert.commandWorked(
    coll.runCommand("insert", {documents: [{_id: largeDocIDs[0], longString: longString}]}),
).operationTime;
jsTest.log.info("First insert timestamp: " + tojson(firstInsertTimestamp));

const secondInsertTimestamp = assert.commandWorked(
    coll.runCommand("insert", {documents: [{_id: largeDocIDs[1], longString: longString}]}),
).operationTime;
jsTest.log.info("Second insert timestamp: " + tojson(secondInsertTimestamp));

// Check inserts exists
assert.soon(() => {
    const foundCount = restartedPrimaryOplog
        .find({ns: "test.markers"}, {"o._id": 1})
        .toArray()
        .filter((e) => largeDocIDs.includes(e.o._id)).length;
    return foundCount == 2;
});

// Insert some more docs to get over sampling threshold
// Record count must be at least
// (kRandomSamplesPerMarker=10 * kMinSampleRatioForRandCursor=20 * minOplogTruncationPoints=2) = 400
for (let i = 0; i < 400; i++) {
    coll.insert({_id: nextId++});
}
jsTest.log.info("Inserted up to id: ", nextId - 1);

// Take a checkpoint
restartedPrimary.getDB("admin").runCommand({fsync: 1});

// Assert no marker generation method is chosen while the thread is paused
let method = assert.commandWorked(restartedPrimary.adminCommand({serverStatus: 1})).oplogTruncation
    .processingMethod;
assert.eq(method, undefined);

// Resume oplog truncate marker creation
jsTest.log.info("Resuming oplog truncate marker creation.");
assert.commandWorked(
    restartedPrimary.adminCommand({configureFailPoint: "hangDuringOplogSampling", mode: "off"}),
);

// Verify sampling is selected as the marker generation method
let truncationStatus;
assert.soon(() => {
    truncationStatus = assert.commandWorked(
        restartedPrimary.adminCommand({serverStatus: 1}),
    ).oplogTruncation;
    return truncationStatus.processingMethod !== undefined;
}, "Oplog truncation marker generation method was never chosen");
assert.eq(truncationStatus.processingMethod, "sampling");
jsTest.log.info("Sampling selected as the marker generation method!");

assert.gt(truncationStatus.minBytesPerMarker, 0, tojson(truncationStatus));

// Verify truncate markers are created and logged
checkLog.containsJson(restartedPrimary, 22382); // Log ID: Oplog truncate markers calculated

// Insert additional records to trigger truncation
jsTest.log.info("Inserting records to trigger truncation");
for (let i = 0; i < 50; i++) {
    coll.insert({_id: nextId++, longString: longString});
}

restartedPrimary.getDB("admin").runCommand({fsync: 1});

// Wait for truncation to occur
// Verify large documents inserted during initial sampling are eventually truncated from the oplog
jsTest.log.info("Waiting for truncation to occur");
assert.soon(() => {
    try {
        const foundLargeDoc = restartedPrimaryOplog
            .find({ns: "test.markers"}, {"o._id": 1})
            .toArray()
            .some((e) => largeDocIDs.includes(e.o._id));
        return !foundLargeDoc;
    } catch (e) {
        if (e.code !== ErrorCodes.CappedPositionLost) {
            throw e;
        }
        print(
            `Failed to fully iterate over collection due to conflict with oplog cap maintainer, retrying`,
        );
        return false;
    }
});

jsTest.log.info("Test complete. Stopping replica set.");
rst.stopSet();
