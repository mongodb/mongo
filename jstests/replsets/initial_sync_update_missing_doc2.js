/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source; and
 * 3) fetches and applies operations from the source after op_start1.
 *
 * This test updates and deletes a document on the source between phases 1 and 2. The
 * secondary will initially fail to apply the update operation in phase 3 and subsequently have
 * to attempt to check the source for a new copy of the document. Before the secondary checks
 * the source, we insert a new copy of the document on the source so that the secondary can fetch
 * it.
 *
 */

(function() {
    load("jstests/replsets/libs/initial_sync_update_missing_doc.js");
    load("jstests/libs/check_log.js");

    var name = 'initial_sync_update_missing_doc2';
    var replSet = new ReplSetTest({
        name: name,
        nodes: 1,
    });

    replSet.startSet();
    replSet.initiate();

    const primary = replSet.getPrimary();
    const dbName = 'test';

    var coll = primary.getDB(dbName).getCollection(name);
    assert.commandWorked(coll.insert({_id: 0, x: 1}));

    // Add a secondary node with priority: 0 and votes: 0 so that we prevent elections while
    // it is syncing from the primary.
    const secondaryConfig = {rsConfig: {votes: 0, priority: 0}};
    const secondary = reInitiateSetWithSecondary(replSet, secondaryConfig);

    // Update and remove document on primary.
    updateRemove(coll, {_id: 0});

    turnOffHangBeforeCopyingDatabasesFailPoint(secondary);

    // Re-insert deleted document on the sync source. The secondary should be able to fetch and
    // insert this document after failing to apply the udpate.
    assert.commandWorked(coll.insert({_id: 0, x: 3}));

    var res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1, initialSync: 1}));
    assert.eq(res.initialSyncStatus.fetchedMissingDocs, 0);
    var firstOplogEnd = res.initialSyncStatus.initialSyncOplogEnd;

    // Temporarily increase log levels so that we can see the 'Inserted missing document' log line.
    secondary.getDB('test').setLogLevel(1, 'replication');
    turnOffHangBeforeGettingMissingDocFailPoint(primary, secondary, name, 1 /* numInserted */);
    secondary.getDB('test').setLogLevel(0, 'replication');

    finishAndValidate(replSet, name, firstOplogEnd, 1 /* numInserted */, 1 /* numDocuments */);

    replSet.stopSet();

})();
