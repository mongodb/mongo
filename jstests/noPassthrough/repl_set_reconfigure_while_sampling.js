/**
 * Checks that, when async oplog sampling enabled, we can reconfigure/initiate the replica set.
 *
 * @tags: [requires_replication, requires_persistence]
 */

function samplingIsIncomplete(primary) {
    const status = primary.getDB("local").serverStatus();
    assert.commandWorked(status);
    jsTestLog(status.oplogTruncation);
    return (!status.oplogTruncation.hasOwnProperty("processingMethod") ||
            status.oplogTruncation.processingMethod == "in progress");
}

// Initialize a single-node replica set with a slow oplog that doesn't truncate.
const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            "oplogSamplingAsyncEnabled": true,
            useSlowCollectionTruncateMarkerScanning: true,
            "failpoint.hangOplogCapMaintainerThread": tojson({mode: "alwaysOn"}),
        },
    },
});
rst.startSet();
rst.initiate();

// Insert initial documents.
jsTestLog("Inserting initial set of documents into the collection.");
for (let i = 0; i < 100; i++) {
    rst.getPrimary().getDB("test").getCollection("markers").insert({a: i});
}

// Stop and restart the replica set.
rst.stopSet(null, true);
rst.startSet(null, true);
jsTestLog("Replica set restarted.");

// We have 100 oplog entries to sample, 1 second each.
const primary = rst.getPrimary();
assert(samplingIsIncomplete(primary));

// Add a member and reconfigure.
const newNode = rst.add({
    setParameter: {
        "oplogSamplingAsyncEnabled": true,
    },
});
rst.reInitiate();

// Add a new member and do a write that requires acknowledgement from both nodes.
assert.commandWorked(
    rst
        .getPrimary()
        .getDB("test")
        .getCollection("markers")
        .insert({a: 1}, {writeConcern: {w: 2}}),
);

// We were able to add a new member and we're still sampling.
assert(samplingIsIncomplete(primary));

rst.stopSet();
