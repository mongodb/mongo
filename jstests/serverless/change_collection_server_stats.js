/**
 * Tests that FTDC collects information about the change collection, including its purging job.
 * @tags: [ requires_fcv_62 ]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/libs/ftdc.js');

const kExpiredChangeRemovalJobSleepSeconds = 5;
const kExpireAfterSeconds = 1;

const rst = new ReplSetTest({nodes: 1});
rst.startSet({
    setParameter: {
        featureFlagServerlessChangeStreams: 1,
        changeCollectionRemoverJobSleepSeconds: kExpiredChangeRemovalJobSleepSeconds,
    }
});
rst.initiate();

const primary = rst.getPrimary();
const adminDb = primary.getDB('admin');
const testDb = primary.getDB(jsTestName());
const changeCollection = primary.getDB("config").system.change_collection;

// Enable change streams to ensure the creation of change collections if run in serverless mode.
assert.commandWorked(adminDb.runCommand({setChangeStreamState: 1, enabled: true}));
assert.soon(() => {
    // Ensure that server status diagnostics is collecting change collection statistics.
    const serverStatusDiagnostics = verifyGetDiagnosticData(adminDb).serverStatus;
    return serverStatusDiagnostics.hasOwnProperty('changeCollections') &&
        serverStatusDiagnostics.changeCollections.hasOwnProperty('purgingJob');
});
const diagnosticsBeforeTestCollInsertions =
    verifyGetDiagnosticData(adminDb).serverStatus.changeCollections.purgingJob;

// Create collection and insert sample data.
assert.commandWorked(testDb.createCollection("testColl"));
const numberOfDocuments = 1000;
for (let i = 0; i < numberOfDocuments; i++) {
    assert.commandWorked(testDb.testColl.insert({x: i}));
}
const wallTimeOfTheFirstOplogEntry =
    new NumberLong(changeCollection.find().sort({wall: 1}).limit(1).next().wall.getTime());
const estimatedToBeRemovedDocsSize = changeCollection.find()
                                         .sort({wall: -1})
                                         .skip(1)
                                         .toArray()
                                         .map(doc => Object.bsonsize(doc))
                                         .reduce((acc, size) => acc + size, 0);
assert.gt(estimatedToBeRemovedDocsSize, 0);

// Set the 'expireAfterSeconds' to 'kExpireAfterSeconds'.
assert.commandWorked(adminDb.runCommand(
    {setClusterParameter: {changeStreams: {expireAfterSeconds: kExpireAfterSeconds}}}));

// Ensure purging job deletes the expired oplog entries about insertion into test collection.
assert.soon(() => {
    // All change collection entries are removed but one.
    return changeCollection.count() === 1;
});

// Ensure that FTDC collected the purging job information of the change collection.
assert.soon(() => {
    const diagnosticsAfterTestCollInsertions =
        verifyGetDiagnosticData(adminDb).serverStatus.changeCollections.purgingJob;

    return diagnosticsAfterTestCollInsertions.totalPass >
        diagnosticsBeforeTestCollInsertions.totalPass &&
        diagnosticsAfterTestCollInsertions.scannedCollections >
        diagnosticsBeforeTestCollInsertions.scannedCollections &&
        diagnosticsAfterTestCollInsertions.bytesDeleted >
        diagnosticsBeforeTestCollInsertions.bytesDeleted + estimatedToBeRemovedDocsSize &&
        diagnosticsAfterTestCollInsertions.docsDeleted >
        diagnosticsBeforeTestCollInsertions.docsDeleted + numberOfDocuments - 1 &&
        diagnosticsAfterTestCollInsertions.maxStartWallTimeMillis.tojson() >=
        wallTimeOfTheFirstOplogEntry.tojson() &&
        diagnosticsAfterTestCollInsertions.timeElapsedMillis >
        diagnosticsBeforeTestCollInsertions.timeElapsedMillis;
});

rst.stopSet();
}());
