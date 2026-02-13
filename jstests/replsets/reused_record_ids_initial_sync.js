/**
 * Validate consistency of initial sync when recordIds have been reused
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 * ]
 */

import {validateShowRecordIdReplicatesAcrossNodes} from "jstests/libs/collection_write_path/replicated_record_ids_utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForState} from "jstests/replsets/rslib.js";

/*
 * Fetches the 'lastCommittedOpTime' field of the given node.
 */
function getLastCommittedOpTime(conn) {
    const replSetStatus = assert.commandWorked(conn.adminCommand({replSetGetStatus: 1}));
    return replSetStatus.optimes.lastCommittedOpTime;
}

function checkRecordIdsResult(arr) {
    for (let i in arr) {
        const doc = arr[i];
        const rid = Number(i) + 1;
        assert(
            doc["$recordId"] == rid,
            "Expected record Id failed, doc rid=" +
                doc["$recordId"] +
                " expected rid=" +
                rid +
                " documents: " +
                tojson(arr),
        );
    }
}

const dbName = "test";
const replRidCollName = "replRecIdColl";

const rst = new ReplSetTest({
    // nodes: 1,
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {
        setParameter: {
            // On restart we will have no history to consult to figure out the highest leaf node.
            minSnapshotHistoryWindowInSeconds: 0,
        },
    },
});
rst.startSet();
rst.initiate();
let primary = rst.getPrimary();
let primDB = primary.getDB(dbName);
const secondary = rst.getSecondary();

primDB.runCommand({create: replRidCollName});

assert.commandWorked(primDB.runCommand({insert: replRidCollName, documents: [{"_id": 1}], writeConcern: {w: 2}}));

// Wait for knowledge of the last commit point to advance to the last write on the primary and
// secondary.
rst.awaitLastOpCommitted(undefined, [primary, secondary]);
const firstCommitPoint = getLastCommittedOpTime(primary);

jsTestLog("Add new replica to the ReplSet");
// Add a third node to the replica set, force it to sync from the secondary, and have it hang in the
// middle of initial sync.
const initialSyncNode = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
        "failpoint.forceSyncSourceCandidate": tojson({mode: "alwaysOn", data: {hostAndPort: secondary.name}}),
        "failpoint.initialSyncHangAfterGettingBeginApplyingTimestamp": tojson({mode: "alwaysOn"}),
    },
});

const hangInitialSyncHangAfterGettingBeginApplyingTimestamp = configureFailPoint(
    initialSyncNode,
    "initialSyncHangAfterGettingBeginApplyingTimestamp",
);

jsTestLog("Waiting for new replica node to reach initial sync state");
rst.reInitiate();
rst.waitForState(initialSyncNode, ReplSetTest.State.STARTUP_2);

// Hang the initial sync node after it sets 'beginApplyingTimestamp' to ensure that the node will
// not set 'stopTimestamp' until after we perform the writes.
hangInitialSyncHangAfterGettingBeginApplyingTimestamp.wait();

jsTestLog("Inserting document into primary.");
assert.commandWorked(
    primDB.runCommand({insert: replRidCollName, documents: [{"_id": 2, "a": 2}], writeConcern: {w: 2}}),
);

jsTestLog("Updating document on primary.");
assert.commandWorked(
    primDB.runCommand({
        update: replRidCollName,
        updates: [
            {
                q: {_id: 2},
                u: {$set: {"a": 3}},
            },
        ],
        writeConcern: {w: 2},
    }),
);

// Deleting the documents on the primary to reuse the Record Id later after restarting
jsTestLog("Deleting document from primary.");
assert.commandWorked(
    primDB.runCommand({
        delete: replRidCollName,
        deletes: [{q: {_id: 2}, limit: 1}],
        writeConcern: {w: 2},
    }),
);

// Wait for knowledge of the last commit point to advance to the last write on the primary and
// secondary.
rst.awaitLastOpCommitted(undefined, [primary, secondary]);

jsTestLog("Restarting primary node.");
rst.stop(primary, undefined, undefined, {forRestart: true});
primary = rst.start(primary, undefined, true /* restart */);

jsTestLog("Forcing primary node to Step Up.");
assert.soonNoExcept(() => {
    const res = primary.adminCommand({replSetStepUp: 1, force: true});
    return res.ok === 1;
});

jsTestLog("Wait for primary node to be ready.");
primary = rst.getPrimary();
primDB = primary.getDB(dbName);

jsTestLog("Re-inserting document on primary node");
// Re-inserting the document. This operation cause the reuse of the record ID as there is no
// highest leaf node.
assert.commandWorked(
    primDB.runCommand({insert: replRidCollName, documents: [{"_id": 3, "a": 4}], writeConcern: {w: 2}}),
);
assert.eq(primDB[replRidCollName].count(), 2);

jsTestLog("Verifying the recordId has been reused");
checkRecordIdsResult(primDB[replRidCollName].find().showRecordId().toArray());

// Wait for knowledge of the last commit point to advance to the last write on the primary and
// secondary.
rst.awaitLastOpCommitted(undefined, [primary, secondary]);
const secondCommitPointPrimary = getLastCommittedOpTime(primary);
const secondCommitPointSecondary = getLastCommittedOpTime(secondary);

// Verify that the commit point has advanced on the primary and secondary.
assert.eq(1, rs.compareOpTimes(secondCommitPointPrimary, firstCommitPoint));
assert.eq(1, rs.compareOpTimes(secondCommitPointSecondary, firstCommitPoint));

jsTestLog("Allowing new replica node to proceed with initial sync");
hangInitialSyncHangAfterGettingBeginApplyingTimestamp.off();

jsTestLog("Wait for new replica node to reach SECONDARY state");
waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);
rst.awaitReplication();

jsTestLog("Validate new replica node");
const initialSyncNodeDB = initialSyncNode.getDB(dbName);
const validateRes = assert.commandWorked(initialSyncNodeDB.runCommand({validate: replRidCollName}));
assert(validateRes.valid, "Validate failed on new replica node: " + tojson(validateRes));

jsTestLog("Validate data across all nodes that all nodes");
// Confirms data returned from a full collection scan yields the same results across all nodes
// (including recordId).
validateShowRecordIdReplicatesAcrossNodes(rst.nodes, dbName, replRidCollName);

rst.stopSet();
