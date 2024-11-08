/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source; and
 * 3) fetches and applies operations from the source after op_start1.
 *
 * This test upserts documents with both the "update" and "applyOps" commands on the source between
 * phases 2 and 3; these operations should be treated as inserts on the syncing node and applied
 * successfully.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    finishAndValidate,
    reInitiateSetWithSecondary
} from "jstests/replsets/libs/initial_sync_update_missing_doc.js";

const replSet = new ReplSetTest({nodes: 1});

replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const dbName = 'test';
const collectionName = jsTestName();

assert.commandWorked(primary.getDB(dbName).createCollection(collectionName));
const coll = primary.getDB(dbName).getCollection(collectionName);

// Add a secondary node with priority: 0 and votes: 0 so that we prevent elections while
// it is syncing from the primary.
const secondaryConfig = {
    rsConfig: {votes: 0, priority: 0}
};
const secondary = reInitiateSetWithSecondary(replSet, secondaryConfig);

jsTestLog("Allow initial sync to finish cloning collections");

let failPoint = configureFailPoint(secondary, 'initialSyncHangAfterDataCloning');
assert.commandWorked(secondary.getDB('admin').runCommand(
    {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));
failPoint.wait();

jsTestLog('Use both "update" and "applyOps" to upsert doc on primary');

let documentIdCounter = 0;
let numDocuments = 0;

assert.commandWorked(coll.update({_id: documentIdCounter}, {x: 1}, {upsert: true}));
documentIdCounter++;
numDocuments++;

function applyOps({documentId}) {
    let command = {
        applyOps: [
            {op: "u", ns: coll.getFullName(), o2: {_id: documentId}, o: {$v: 2, diff: {u: {x: 1}}}}
        ]
    };

    assert.commandWorked(primary.getDB(dbName).runCommand(command));
}

/* The interesting scenario for (default) alwaysUpsert: false is if the document is deleted on
 * the primary after updating. When the secondary attempts to apply the oplog entry during initial
 * sync, it will fail to update. Ensure that initial sync proceeds anyway.
 */
coll.insertOne({_id: documentIdCounter});
applyOps({documentId: documentIdCounter});
coll.deleteOne({_id: documentIdCounter});
// Don't increment numDocuments, since we deleted the document we just inserted.
documentIdCounter++;

jsTestLog("Allow initial sync to finish fetching and replaying oplog");

failPoint.off();

finishAndValidate(replSet, collectionName, numDocuments);

replSet.stopSet();
