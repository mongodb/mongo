"use strict";
/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source; and
 * 3) fetches and applies operations from the source after op_start1.
 *
 * This library is used to delete documents on the sync source between the first two phases so
 * that the secondary will fail to apply the update operation in phase three.
 */

// reInitiate the replica set with a secondary node, which will go through initial sync. This
// function will hand the secondary in initial sync. turnOffHangBeforeCopyingDatabasesFailPoint
// must be called after reInitiateSetWithSecondary, followed by
// turnOffHangBeforeGettingMissingDocFailPoint.
var reInitiateSetWithSecondary = function(replSet, secondaryConfig) {

    const secondary = replSet.add(secondaryConfig);
    secondary.setSlaveOk();

    // Make the secondary hang after retrieving the last op on the sync source but before
    // copying databases.
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'alwaysOn'}));
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeGettingMissingDocument', mode: 'alwaysOn'}));

    // Skip clearing initial sync progress after a successful initial sync attempt so that we
    // can check initialSyncStatus fields after initial sync is complete.
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: 'skipClearInitialSyncState', mode: 'alwaysOn'}));

    replSet.reInitiate();

    // Wait for fail point message to be logged.
    checkLog.contains(secondary,
                      'initial sync - initialSyncHangBeforeCopyingDatabases fail point enabled');

    return secondary;

};

// Must be called after reInitiateSetWithSecondary. Turns off the
// initialSyncHangBeforeCopyingDatabases fail point so that the secondary will start copying all
// non-local databases.
var turnOffHangBeforeCopyingDatabasesFailPoint = function(secondary) {

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

    // The following checks assume that we have updated and deleted a document on the sync source
    // that the secondary will try to update in phase 3.
    checkLog.contains(secondary, 'update of non-mod failed');
    checkLog.contains(secondary, 'Fetching missing document');
    checkLog.contains(
        secondary, 'initial sync - initialSyncHangBeforeGettingMissingDocument fail point enabled');
};

// Must be called after turnOffHangBeforeCopyingDatabasesFailPoint. Turns off the
// initialSyncHangBeforeGettingMissingDocument fail point so that the secondary can check if the
// sync source has the missing document.
var turnOffHangBeforeGettingMissingDocFailPoint = function(primary, secondary, name, numInserted) {

    if (numInserted === 0) {
        // If we did not re-insert the missing document, insert an arbitrary document to move
        // forward minValid even though the document was not found.
        assert.commandWorked(
            primary.getDB('test').getCollection(name + 'b').insert({_id: 1, y: 1}));
    }

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeGettingMissingDocument', mode: 'off'}));

    // If we've re-inserted the missing document between secondaryHangsBeforeGettingMissingDoc and
    // this function, the secondary will insert the missing document after it fails the update.
    // Otherwise, it will fail to fetch anything from the sync source because the document was
    // deleted.
    if (numInserted > 0) {
        checkLog.contains(secondary, 'Inserted missing document');
    } else {
        checkLog.contains(
            secondary, 'Missing document not found on source; presumably deleted later in oplog.');
    }
    checkLog.contains(secondary, 'initial sync done');

};

var finishAndValidate = function(replSet, name, firstOplogEnd, numInserted, numDocuments) {

    replSet.awaitReplication();
    replSet.awaitSecondaryNodes();
    const dbName = 'test';
    const secondary = replSet.getSecondary();

    assert.eq(numDocuments,
              secondary.getDB(dbName).getCollection(name).find().itcount(),
              'documents successfully synced to secondary');

    const res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));

    // If we haven't re-inserted any documents after deleting them, the fetch count is 0 because we
    // are unable to get the document from the sync source.
    assert.eq(res.initialSyncStatus.fetchedMissingDocs, numInserted);

    const finalOplogEnd = res.initialSyncStatus.initialSyncOplogEnd;

    if (numInserted > 0) {
        assert.neq(firstOplogEnd,
                   finalOplogEnd,
                   "minValid was not moved forward when missing document was fetched");
    } else {
        assert.eq(firstOplogEnd,
                  finalOplogEnd,
                  "minValid was moved forward when missing document was not fetched");
    }

    assert.eq(0,
              secondary.getDB('local')['temp_oplog_buffer'].find().itcount(),
              "Oplog buffer was not dropped after initial sync");

};

var updateRemove = function(sessionColl, query) {
    assert.commandWorked(sessionColl.update(query, {x: 2}, {upsert: false}));
    assert.commandWorked(sessionColl.remove(query, {justOne: true}));
};

var insertUpdateRemoveLarge = function(sessionColl, query) {
    const kSize10MB = 10 * 1024 * 1024;
    const longString = "a".repeat(kSize10MB);
    assert.commandWorked(sessionColl.insert({x: longString}));
    assert.commandWorked(sessionColl.update(query, {x: longString}, {upsert: false}));
    assert.commandWorked(sessionColl.remove(query, {justOne: true}));
};
