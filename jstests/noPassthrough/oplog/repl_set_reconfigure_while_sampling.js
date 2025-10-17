/**
 * Checks that, when async oplog sampling enabled, we can reconfigure/initiate the replica set.
 *
 * @tags: [requires_replication, requires_persistence]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

function samplingIsIncomplete(primary) {
    const status = primary.getDB("local").serverStatus();
    assert.commandWorked(status);
    jsTest.log.info(status.oplogTruncation);
    return (
        !status.oplogTruncation.hasOwnProperty("processingMethod") ||
        status.oplogTruncation.processingMethod == "in progress"
    );
}

// Initialize a single-node replica set with a slow oplog that doesn't truncate.
const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            useSlowCollectionTruncateMarkerScanning: true,
            "failpoint.hangOplogCapMaintainerThread": tojson({mode: "alwaysOn"}),
        },
    },
});
rst.startSet();
rst.initiate();

// Insert initial documents.
jsTest.log.info("Inserting initial set of documents into the collection.");
for (let i = 0; i < 100; i++) {
    rst.getPrimary().getDB("test").getCollection("markers").insert({a: i});
}

// Stop and restart the replica set.
rst.stopSet(null, true);
rst.startSet(null, true);
jsTest.log.info("Replica set restarted.");

// We have 100 oplog entries to sample, 1 second each.
const primary = rst.getPrimary();
assert(samplingIsIncomplete(primary));

// Add a member and reconfigure.
const newNode = rst.add({
    rsConfig: {priority: 0, votes: 0},
});
rst.reInitiate();

// We were able to add a new member and we're still sampling.
assert(samplingIsIncomplete(primary));
assert(samplingIsIncomplete(newNode));

rst.stopSet();
