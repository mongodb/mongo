/**
 * Test that reused replicated records Ids does not cause inconsistencies between primary and
 * secondary.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 *   # TODO (SERVER-89640): Remove tag.
 *   # Incompatible with the recordIdsReplicated:true builder, as it sets the option
 *   # on all collections.
 *   exclude_when_record_ids_replicated
 * ]
 */
import {
    validateShowRecordIdReplicatesAcrossNodes
} from "jstests/libs/collection_write_path/replicated_record_ids_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {stopReplicationOnSecondaries} from "jstests/libs/write_concern_util.js";

function checkRecordIdsResult(arr) {
    for (let i in arr) {
        const doc = arr[i];
        assert(doc['$recordId'] = i);
    }
}

const numDocs = 10;    // Number of documents to insert, delete and re-insert
const idOffset = 500;  // The start of the new set of `_id` when reinserting

const rst = new ReplSetTest({
    nodes: [
        {},
        {rsConfig: {priority: 0}},  // Prevent secondary from stepping up.
    ],
    nodeOptions: {
        setParameter: {
            // On restart we will have no history to consult to figure out the highest leaf node.
            minSnapshotHistoryWindowInSeconds: 0,
        }
    }
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let secondary = rst.getSecondaries()[0];

const dbName = 'test';
const replRidCollName = 'replRecIdColl';

let primDB = primary.getDB(dbName);

// Create a collection with the param `recordsIdsReplicated` param set.
primDB.runCommand({create: replRidCollName, recordIdsReplicated: true});

jsTestLog("Blocking replication to secondary.");
stopReplicationOnSecondaries(rst);

// Inserting the documents
jsTestLog("Inserting documents into primary.");
for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(primDB.runCommand(
        {insert: replRidCollName, documents: [{"_id": i}], writeConcern: {w: 1}}));
}

jsTestLog("Checking records IDs on primary.");
checkRecordIdsResult(primDB[replRidCollName].find().showRecordId().toArray());

// Deleting the documents on the primary to reuse the Record Id later after restarting
jsTestLog("Deleting documents from primary.");
for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(primDB.runCommand({
        delete: replRidCollName,
        deletes: [
            {q: {_id: i}, limit: 1},
        ],
        writeConcern: {w: 1}
    }));
}

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

jsTestLog("Re-inserting documents on primary node");
// Re-inserting the documents. This operation cause the reuse of the record IDs as there is no
// highest leaf node. During replication, secondaries apply oplog entries concurrently in batches.
// A batch of oplog entries is distributed among multiple applier workers, who process these entries
// in parallel. Entries are assigned to workers based on their `_id` field. To maximize parallelism
// and improve the likelihood that these operations are processed by different workers (and executed
// concurrently with the earlier inserts and deletes), we use different `_id` values for re-inserted
// documents.
for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(primDB.runCommand(
        {insert: replRidCollName, documents: [{"_id": idOffset + i}], writeConcern: {w: 1}}));
}
assert.eq(primDB[replRidCollName].count(), numDocs);

jsTestLog("Verifying the recordIds have been reused");
checkRecordIdsResult(primDB[replRidCollName].find().showRecordId().toArray());

jsTestLog("Restarting secondary node.");
rst.stop(secondary, undefined, undefined, {forRestart: true});
secondary = rst.start(secondary, undefined, true /* restart */);
const secDB = secondary.getDB(dbName);

// Wait secondary to be ready
jsTestLog("Waiting secondary node to be ready");
rst.awaitReplication();
rst.awaitSecondaryNodes();

jsTestLog("Verifying documents on secondary");
// Secondary should have the same number of documents as Primary
assert.eq(secDB[replRidCollName].count(), numDocs);
// Ensure that the on disk data on both nodes has the same recordIds.
validateShowRecordIdReplicatesAcrossNodes(rst.nodes, dbName, replRidCollName);

rst.stopSet();
