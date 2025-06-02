/**
 * Verify that repl portion of serverStatus's metrics section has all the expected fields
 *
 * @tags: [multiversion_incompatible]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Compares only key sets, ignoring the values
function verifySameProperties(actual, expected) {
    // Helper function to get the property keys of an object
    function getKeys(obj) {
        return Object.keys(obj).sort();
    }
    const keys1 = getKeys(actual);
    const keys2 = getKeys(expected);

    // Compare keys and recursively compare their values if they are objects
    for (let i = 0; i < keys1.length; i++) {
        const key = keys1[i];
        assert(expected.hasOwnProperty(key),
               () => (`Server Status metrics repl section has unexpected property ${
                   key}, please fix this test`));
    }
    for (let i = 0; i < keys2.length; i++) {
        const key = keys2[i];
        assert(actual.hasOwnProperty(key),
               () => (`Server Status metrics repl section has no expected property ${key}`));
        // Recursively verify nested objects
        if (typeof actual[key] === 'object' && typeof expected[key] === 'object') {
            verifySameProperties(actual[key], expected[key]);
        }
    }
}
// Set up the replica set.
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

const serverStatusResponse = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
const expectedReplSection = {
    "buffer": {
        "write": {"count": 0, "sizeBytes": 0, "maxSizeBytes": 0},
        "apply": {"count": 0, "sizeBytes": 0, "maxSizeBytes": 0, "maxCount": 0}
    },
    "executor": {
        "pool": {"inProgressCount": 0},
        "queues": {"networkInProgress": 0, "sleepers": 0},
        "unsignaledEvents": 0,
        "shuttingDown": false,
        "networkInterface": "DEPRECATED: getDiagnosticString is deprecated in NetworkInterfaceTL"
    },
    "write": {"batchSize": 0, "batches": {"num": 0, "totalMillis": 0}},
    "apply": {
        "attemptsToBecomeSecondary": 1,
        "batchSize": 0,
        "batches": {"num": 0, "totalMillis": 0},
        "ops": 0
    },
    "heartBeat": {"handleQueueSize": 0, "maxSeenHandleQueueSize": 0},
    "initialSync": {"completed": 0, "failedAttempts": 0, "failures": 0},
    "network": {
        "bytes": 0,
        "getmores": {"num": 0, "totalMillis": 0, "numEmptyBatches": 0},
        "notPrimaryLegacyUnacknowledgedWrites": 0,
        "notPrimaryUnacknowledgedWrites": 0,
        "oplogGetMoresProcessed": {"num": 0, "totalMillis": 0},
        "ops": 0,
        "readersCreated": 0,
        "replSetUpdatePosition": {"num": 0}
    },
    "reconfig": {"numAutoReconfigsForRemovalOfNewlyAddedFields": 0},
    "stateTransition": {
        "lastStateTransition": "stepUp",
        "totalOperationsKilled": 0,
        "totalOperationsRunning": 3,
        "totalOperationsKilledByIntentRegistry": 0
    },
    "syncSource": {
        "numSelections": 1,
        "numSyncSourceChangesDueToSignificantlyCloserNode": 0,
        "numTimesChoseDifferent": 0,
        "numTimesChoseSame": 0,
        "numTimesCouldNotFind": 0
    },
    "timestamps": {"oldestTimestamp": 0},
    "waiters": {
        "opTime": 0,
        "replication": 0,
        "replCoordMutexTotalWaitTimeInOplogServerStatusMillis": 0,
        "numReplCoordMutexAcquisitionsInOplogServerStatus": 0
    }
};
const expectedIntentRegistrySection = {
    "intentsDeclaredForREAD": 0,
    "intentsDeclaredForWRITE": 0,
    "intentsDeclaredForLOCAL_WRITE": 0,
    "intentsDeclaredForPREPARED_TRANSACTION": 0
};
assert(serverStatusResponse.metrics.hasOwnProperty("repl"),
       () => (`The serverStatus response did not have the repl \
section: \n${tojson(serverStatusResponse)}`));
jsTestLog("printing server status response");
assert(serverStatusResponse.hasOwnProperty("intentRegistry"),
       () => (`The serverStatus response did not have the intentRegistry \
section: \n${tojson(serverStatusResponse)}`));
jsTestLog(serverStatusResponse);
verifySameProperties(serverStatusResponse.metrics.repl, expectedReplSection);
verifySameProperties(serverStatusResponse.intentRegistry, expectedIntentRegistrySection);
// Stop the replica set.
rst.stopSet();
