/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source; and
 * 3) fetches and applies operations from the source after op_start1.
 *
 * This test updates and deletes a document on the source between phases 1 and 2. The
 * secondary will initially fail to apply the update operation in phase 3 and subsequently have
 * to attempt to check the source for a new copy of the document. The absence of the document on
 * the source indicates that the source is free to ignore the failed update operation.
 */

(function() {
    load("jstests/libs/check_log.js");

    var parameters = TestData.setParameters;
    if (parameters && parameters.indexOf("use3dot2InitialSync=true") != -1) {
        jsTest.log("Skipping this test because use3dot2InitialSync was provided.");
        return;
    }

    var name = 'initial_sync_update_missing_doc1';
    var replSet = new ReplSetTest({
        name: name,
        nodes: [{}, {rsConfig: {arbiterOnly: true}}],
    });

    replSet.startSet();
    replSet.initiate();
    var primary = replSet.getPrimary();

    var coll = primary.getDB('test').getCollection(name);
    assert.writeOK(coll.insert({_id: 0, x: 1}));

    // Add a secondary node but make it hang after retrieving the last op on the source
    // but before copying databases.
    var secondary = replSet.add();
    secondary.setSlaveOk();

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'alwaysOn'}));
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeGettingMissingDocument', mode: 'alwaysOn'}));
    replSet.reInitiate();

    // Wait for fail point message to be logged.
    checkLog.contains(secondary,
                      'initial sync - initialSyncHangBeforeCopyingDatabases fail point enabled');

    assert.writeOK(coll.update({_id: 0}, {x: 2}, {upsert: false, writeConcern: {w: 1}}));
    assert.writeOK(coll.remove({_id: 0}, {justOne: true, writeConcern: {w: 1}}));

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

    checkLog.contains(secondary, 'update of non-mod failed');
    checkLog.contains(secondary, 'Fetching missing document');
    checkLog.contains(
        secondary, 'initial sync - initialSyncHangBeforeGettingMissingDocument fail point enabled');
    var res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1, initialSync: 1}));
    assert.eq(res.initialSyncStatus.fetchedMissingDocs, 0);
    var firstOplogEnd = res.initialSyncStatus.initialSyncOplogEnd;

    // Insert a document to move forward minValid, even though the document was not found.
    assert.writeOK(primary.getDB('test').getCollection(name + 'b').insert({_id: 1, y: 1}));

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeGettingMissingDocument', mode: 'off'}));
    checkLog.contains(secondary,
                      'Missing document not found on source; presumably deleted later in oplog.');
    checkLog.contains(secondary, 'initial sync done');

    replSet.awaitReplication();
    replSet.awaitSecondaryNodes();

    assert.eq(0,
              secondary.getDB('test').getCollection(name).find().itcount(),
              'collection successfully synced to secondary');

    res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1, initialSync: 1}));
    assert.eq(res.initialSyncStatus.fetchedMissingDocs, 1);
    var finalOplogEnd = res.initialSyncStatus.initialSyncOplogEnd;
    assert(!friendlyEqual(firstOplogEnd, finalOplogEnd),
           "minValid was not moved forward when missing document was fetched");

    assert.eq(0,
              secondary.getDB('local')['temp_oplog_buffer'].find().itcount(),
              "Oplog buffer was not dropped after initial sync");

    replSet.stopSet();
})();
