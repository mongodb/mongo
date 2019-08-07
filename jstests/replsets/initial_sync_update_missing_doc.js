/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source; and
 * 3) fetches and applies operations from the source after op_start1.
 *
 * This test updates and deletes a document on the source between phases 1 and 2. The secondary will
 * fail to apply the update operation in phase 3 but initial sync completes nevertheless. The
 * absence of the document on the source indicates that the source is free to ignore the failed
 * update operation.
 */

(function() {
load("jstests/replsets/libs/initial_sync_update_missing_doc.js");
load("jstests/libs/check_log.js");

const replSet = new ReplSetTest({nodes: 1});

replSet.startSet();
replSet.initiate();
const primary = replSet.getPrimary();
const dbName = 'test';
const collectionName = jsTestName();
const coll = primary.getDB(dbName).getCollection(collectionName);
assert.commandWorked(coll.insert({_id: 0, x: 1}));

// Add a secondary node with priority: 0 and votes: 0 so that we prevent elections while
// it is syncing from the primary.
const secondaryConfig = {
    rsConfig: {votes: 0, priority: 0}
};
const secondary = reInitiateSetWithSecondary(replSet, secondaryConfig);

// Update and remove document on primary.
updateRemove(coll, {_id: 0});
turnOffHangBeforeCopyingDatabasesFailPoint(secondary);
finishAndValidate(replSet, collectionName, 0 /* numDocuments */);

replSet.stopSet();
})();
