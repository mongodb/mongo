/**
 * Checks that when async oplog sampling enabled, any data that is written in parallel with initial
 * sampling eventually gets truncated. This test also checks that the oplog sampling and initial
 * marker creation does not block startup and can successfully complete post-startup.
 *
 * @tags: [requires_replication, requires_persistence]
 */
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";

// Constants for replica set and test configuration
const oplogSizeMB = 1;                      // Small oplog size in MB
const longString = "a".repeat(450 * 1024);  // Large document size (~500KB)
let nextId = 0;                             // Tracks the next `_id` for inserts

// Initialize a single-node replica set with a small oplog size
const rst = new ReplSetTest({
    oplogSize: oplogSizeMB,
    nodes: 1,
    // Set the syncdelay to 1s to speed up checkpointing.
    nodeOptions: {
        syncdelay: 1,
        setParameter: {
            oplogSamplingAsyncEnabled: true,
            logComponentVerbosity: tojson({storage: 1}),
            minOplogTruncationPoints: 2,
            internalQueryExecYieldPeriodMS: 86400000,  // Disable yielding
        },
    },
});
rst.startSet();
rst.initiate();

// Insert initial documents
jsTestLog("Inserting initial set of documents into the collection.");
for (let i = 0; i < 10; i++) {
    rst.getPrimary().getDB("test").getCollection("markers").insert({_id: nextId++});
}

// Stop and restart the replica set
rst.stopSet(null, true);
jsTestLog("Replica set stopped for restart.");
clearRawMongoProgramOutput();

rst.startSet({
    restart: true,
    setParameter: {
        "failpoint.hangDuringOplogSampling": tojson({mode: "alwaysOn"}),
        "oplogSamplingAsyncEnabled": true,
    },
});  // Restart replica set
const restartedPrimary = rst.getPrimary();
const restartedPrimaryOplog = restartedPrimary.getDB("local").getCollection("oplog.rs");
jsTestLog("Replica set restarted.");

// // Verify that the oplog cap maintainer thread is paused.
assert.commandWorked(
    restartedPrimary.adminCommand({
        waitForFailPoint: "hangDuringOplogSampling",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

// Test inserts while truncate marker creation process is paused
jsTestLog("Testing oplog truncation logic with new inserts...");
const coll = restartedPrimary.getDB("test").markers;
const largeDocIDs = [nextId++, nextId++];

// Insert large documents
const firstInsertTimestamp =
    assert
        .commandWorked(
            coll.runCommand("insert", {documents: [{_id: largeDocIDs[0], longString: longString}]}),
            )
        .operationTime;
jsTestLog("First insert timestamp: " + tojson(firstInsertTimestamp));

const secondInsertTimestamp =
    assert
        .commandWorked(
            coll.runCommand("insert", {documents: [{_id: largeDocIDs[1], longString: longString}]}),
            )
        .operationTime;
jsTestLog("Second insert timestamp: " + tojson(secondInsertTimestamp));

// Check inserts exists


assert.soon(() => {
    let foundCount = 0;
    const cursor = restartedPrimaryOplog.find({ns: "test.markers"});
    while (cursor.hasNext()) {
        const entry = cursor.next();
        jsTestLog("Checking " + tojson(entry));
        largeDocIDs.forEach((id) => {
            if (id == entry.o["_id"]) {
                foundCount++;
            }
        });
    }
    return foundCount == 2;
});

// Take a checkpoint
restartedPrimary.getDB("admin").runCommand({fsync: 1});

// Verify truncate marker creation resumes post-startup
checkLog.containsJson(restartedPrimary, 8423403);  // Log ID for startup finished

// Resume oplog truncate marker creation
jsTestLog("Resuming oplog truncate marker creation.");
assert.commandWorked(
    restartedPrimary.adminCommand({configureFailPoint: "hangDuringOplogSampling", mode: "off"}));

// Verify truncate markers are created and logged
checkLog.containsJson(restartedPrimary, 22382);  // Log ID: Oplog truncate markers calculated

// Insert additional records to trigger truncation
for (let i = 0; i < 50; i++) {
    coll.insert({_id: nextId++, longString: longString});
}

restartedPrimary.getDB("admin").runCommand({fsync: 1});

// Wait for truncation to occur
// Verify large documents inserted during intial sampling are eventually truncated from the oplog
assert.soon(() => {
    const cursor = restartedPrimaryOplog.find({ns: "test.markers"});
    while (cursor.hasNext()) {
        const entry = cursor.next();
        jsTestLog("Checking " + tojson(entry));
        largeDocIDs.forEach((id) => {
            if (id == entry.o["_id"]) {
                return false;
            }
        });
    }
    return true;
});


jsTestLog("Test complete. Stopping replica set.");
rst.stopSet();
