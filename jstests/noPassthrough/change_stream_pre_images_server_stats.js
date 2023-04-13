/**
 * Tests that FTDC collects information about the pre-image collection, including its purging job.
 * @tags: [ requires_replication ]
 */
(function() {
'use strict';

// For verifyGetDiagnosticData.
load('jstests/libs/ftdc.js');

const kExpiredPreImageRemovalJobSleepSeconds = 1;
const kExpireAfterSeconds = 1;

const replicaSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter:
            {expiredChangeStreamPreImageRemovalJobSleepSecs: kExpiredPreImageRemovalJobSleepSeconds}
    }
});

replicaSet.startSet();
replicaSet.initiate();

const primary = replicaSet.getPrimary();
const adminDb = primary.getDB('admin');
const testDb = primary.getDB(jsTestName());

assert.soon(() => {
    // Ensure that server status diagnostics is collecting pre-image collection statistics.
    const serverStatusDiagnostics = verifyGetDiagnosticData(adminDb).serverStatus;
    return serverStatusDiagnostics.hasOwnProperty('changeStreamPreImages') &&
        serverStatusDiagnostics.changeStreamPreImages.hasOwnProperty('purgingJob');
});

const diagnosticsBeforeTestCollModifications =
    verifyGetDiagnosticData(adminDb).serverStatus.changeStreamPreImages.purgingJob;

// Create collection and insert sample data.
assert.commandWorked(
    testDb.createCollection("testColl", {changeStreamPreAndPostImages: {enabled: true}}));
const numberOfDocuments = 100;
for (let i = 0; i < numberOfDocuments; i++) {
    assert.commandWorked(testDb.testColl.insert({x: i}));
}

for (let i = 0; i < numberOfDocuments; i++) {
    assert.commandWorked(testDb.testColl.updateOne({x: i}, {$inc: {y: 1}}));
}

const preImageCollection = primary.getDB('config')['system.preimages'];

const estimatedToBeRemovedDocsSize = preImageCollection.find()
                                         .toArray()
                                         .map(doc => Object.bsonsize(doc))
                                         .reduce((acc, size) => acc + size, 0);
assert.gt(estimatedToBeRemovedDocsSize, 0);

// Set the 'expireAfterSeconds' to 'kExpireAfterSeconds'.
assert.commandWorked(adminDb.runCommand({
    setClusterParameter:
        {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: kExpireAfterSeconds}}}
}));

// Ensure purging job deletes the expired pre-image entries of the test collection.
assert.soon(() => {
    // All entries are removed.
    return preImageCollection.count() === 0;
});

// Ensure that FTDC collected the purging job information of the pre-image collection.
assert.soon(() => {
    const diagnosticsAfterTestCollModifications =
        verifyGetDiagnosticData(adminDb).serverStatus.changeStreamPreImages.purgingJob;

    const totalPassBigger = diagnosticsAfterTestCollModifications.totalPass >
        diagnosticsBeforeTestCollModifications.totalPass;
    const scannedBigger = diagnosticsAfterTestCollModifications.scannedCollections >
        diagnosticsBeforeTestCollModifications.scannedCollections;
    const scannedInternalBigger = diagnosticsAfterTestCollModifications.scannedInternalCollections >
        diagnosticsBeforeTestCollModifications.scannedInternalCollections;
    const bytesEqual = diagnosticsAfterTestCollModifications.bytesDeleted >=
        diagnosticsBeforeTestCollModifications.bytesDeleted + estimatedToBeRemovedDocsSize;
    const docsDeletedEqual = diagnosticsAfterTestCollModifications.docsDeleted >=
        diagnosticsBeforeTestCollModifications.docsDeleted + numberOfDocuments;
    const wallTimeGTE = diagnosticsAfterTestCollModifications.maxStartWallTimeMillis.tojson() >=
        ISODate("1970-01-01T00:00:00.000Z").tojson();
    const timeElapsedGTE = diagnosticsAfterTestCollModifications.timeElapsedMillis >=
        diagnosticsBeforeTestCollModifications.timeElapsedMillis;

    // For debug purposes log which condition failed.
    if (!totalPassBigger) {
        jsTestLog("totalPassBigger failed, retrying");
        return false;
    }
    if (!scannedBigger) {
        jsTestLog("scannedBigger failed, retrying");
        return false;
    }
    if (!scannedInternalBigger) {
        jsTestLog("scannedInternalBigger failed, retrying");
        return false;
    }
    if (!bytesEqual) {
        jsTestLog("bytesEqual) failed, retrying");
        return false;
    }
    if (!docsDeletedEqual) {
        jsTestLog("docsDeletedEqual failed, retrying");
        return false;
    }
    if (!wallTimeGTE) {
        jsTestLog("wallTimeGTE failed, retrying");
        return false;
    }
    if (!timeElapsedGTE) {
        jsTestLog("timeElapsedGTE failed, retrying");
        return false;
    }

    return totalPassBigger && scannedBigger && scannedInternalBigger && bytesEqual &&
        docsDeletedEqual && wallTimeGTE && timeElapsedGTE;
});

replicaSet.stopSet();
}());
