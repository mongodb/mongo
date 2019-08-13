/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source; and
 * 3) fetches and applies operations from the source after op_start1.
 *
 * This test updates and deletes a document on the source between phases 1 and 2. The
 * secondary will initially fail to apply the update operation in phase 3 and subsequently have
 * to attempt to check the source for a new copy of the document. Before the secondary checks the
 * source, we insert a new copy of the document into this collection on the source and mark the
 * collection on which the document to be fetched resides as drop pending, thus effectively
 * renaming the collection but preserving the UUID. This ensures the secondary fetches the
 * document by UUID rather than namespace.
 */

(function() {
load("jstests/libs/check_log.js");
load("jstests/replsets/libs/initial_sync_update_missing_doc.js");
load("jstests/replsets/libs/two_phase_drops.js");  // For TwoPhaseDropCollectionTest.

var name = 'initial_sync_update_missing_doc3';
var replSet = new ReplSetTest({
    name: name,
    nodes: 1,
});

replSet.startSet();
replSet.initiate();
const primary = replSet.getPrimary();
const dbName = 'test';

// Check for 'system.drop' two phase drop support.
if (!TwoPhaseDropCollectionTest.supportsDropPendingNamespaces(replSet)) {
    jsTestLog('Drop pending namespaces not supported by storage engine. Skipping test.');
    replSet.stopSet();
    return;
}

var coll = primary.getDB(dbName).getCollection(name);
assert.commandWorked(coll.insert({_id: 0, x: 1}));

// Add a secondary node with priority: 0 so that we prevent elections while it is syncing
// from the primary.
// We cannot give the secondary votes: 0 because then it will not be able to acknowledge
// majority writes. That means the sync source can immediately drop it's collection
// because it alone determines the majority commit point.
const secondaryConfig = {
    rsConfig: {priority: 0}
};
const secondary = reInitiateSetWithSecondary(replSet, secondaryConfig);

// Update and remove document on primary.
updateRemove(coll, {_id: 0});

turnOffHangBeforeCopyingDatabasesFailPoint(secondary);

// Re-insert deleted document.
assert.commandWorked(coll.insert({_id: 0, x: 3}));
// Mark the collection as drop pending so it gets renamed, but retains the UUID.
assert.commandWorked(primary.getDB('test').runCommand({"drop": name}));

var res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
assert.eq(res.initialSyncStatus.fetchedMissingDocs, 0);

secondary.getDB('test').setLogLevel(1, 'replication');
turnOffHangBeforeGettingMissingDocFailPoint(primary, secondary, name, 1 /* numFetched */);
secondary.getDB('test').setLogLevel(0, 'replication');

replSet.awaitReplication();
replSet.awaitSecondaryNodes();

replSet.stopSet();
})();
