/**
 * Tests insert generating duplicate key during initial sync are skipped.
 *
 * @tags: [requires_fcv_83]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForState} from "jstests/replsets/rslib.js";

const dbName = "test";
const collName = "testColl";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // On restart we will have no history to consult to figure out the highest leaf node.
            minSnapshotHistoryWindowInSeconds: 0,
            "failpoint.overrideRecordIdsReplicatedDefault": "{mode: 'alwaysOn'}",
            "failpoint.automaticallyCollmodToRecordIdsReplicatedFalse": "{mode: 'alwaysOn'}",
        },
    },
});
rst.startSet();
rst.initiate();
let primary = rst.getPrimary();
let primDB = primary.getDB(dbName);

assert.commandWorked(primDB.runCommand({insert: collName, documents: [{"_id": 1}]}));

jsTestLog("Add replica to the ReplSet");
// Add a node to the replica set and have it hang in the middle of initial sync.
const initialSyncNode = rst.add({
    setParameter: {
        "failpoint.initialSyncHangAfterGettingBeginApplyingTimestamp": tojson({mode: "alwaysOn"}),
    },
});

const hangInitialSyncHangAfterGettingBeginApplyingTimestamp = configureFailPoint(
    initialSyncNode,
    "initialSyncHangAfterGettingBeginApplyingTimestamp",
);

jsTestLog("Waiting for new node to reach initial sync state");
rst.reInitiate();
rst.waitForState(initialSyncNode, ReplSetTest.State.STARTUP_2);

// Hang the initial sync node after it sets 'beginApplyingTimestamp' to ensure that the node will
// not set 'stopTimestamp' until after we perform the writes.
hangInitialSyncHangAfterGettingBeginApplyingTimestamp.wait();

jsTestLog("Inserting document into primary.");
assert.commandWorked(primDB.runCommand({insert: collName, documents: [{"_id": 2, "a": 2}]}));

jsTestLog("Updating document on primary.");
assert.commandWorked(
    primDB.runCommand({
        update: collName,
        updates: [
            {
                q: {_id: 2},
                u: {$set: {"a": 3}},
            },
        ],
    }),
);

// Set log level to debug so we log the skipped insert
const initialSyncNodeDB = initialSyncNode.getDB(dbName);
assert.commandWorked(initialSyncNodeDB.setLogLevel(3, "replication"));

jsTestLog("Allowing replica node to proceed with initial sync");
hangInitialSyncHangAfterGettingBeginApplyingTimestamp.off();

jsTestLog("Wait for replica node to reach SECONDARY state");
waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);
rst.awaitReplication();

// Confirm we saw the skipped insert
checkLog.containsJson(initialSyncNode, 8776800);

jsTestLog("Validate new replica node");
const validateRes = assert.commandWorked(initialSyncNodeDB.runCommand({validate: collName}));
assert(validateRes.valid, "Validate failed on replica node: " + tojson(validateRes));

rst.stopSet();
