/**
 * Test to ensure that an operation with a majority write concern waits for drop pending
 * collections, with optimes preceding or equal to the operation's optime, to be reaped.
 */

(function() {
'use strict';

load('jstests/libs/check_log.js');
load('jstests/replsets/libs/two_phase_drops.js');  // For TwoPhaseDropCollectionTest.

// Alias to logging function in two_phase_drops.js
const testLog = TwoPhaseDropCollectionTest._testLog;

/**
 * Ensures that the operation fails with a write concern timeout.
 */
function assertTimeout(result) {
    assert.writeErrorWithCode(result, ErrorCodes.WriteConcernFailed);
    assert(result.hasWriteConcernError(), tojson(result));
    assert(result.getWriteConcernError().errInfo.wtimeout, tojson(result));
}

// Set up a two phase drop test.
let testName = 'drop_collection_two_phase_write_concern';
let dbName = testName;
let collName = 'collToDrop';
let twoPhaseDropTest = new TwoPhaseDropCollectionTest(testName, dbName);

// Initialize replica set.
let replTest = twoPhaseDropTest.initReplSet();

// Check for 'system.drop' two phase drop support.
if (!twoPhaseDropTest.supportsDropPendingNamespaces()) {
    jsTestLog('Drop pending namespaces not supported by storage engine. Skipping test.');
    twoPhaseDropTest.stop();
    return;
}

// Create the collection that will be dropped.
twoPhaseDropTest.createCollection(collName);

const primary = replTest.getPrimary();
const primaryDB = primary.getDB(dbName);
const collForInserts = primaryDB.getCollection('collForInserts');
const writeConcernForSuccessfulOp = {
    w: 'majority',
    wtimeout: replTest.kDefaultTimeoutMS
};
assert.writeOK(collForInserts.insert({_id: 0}, {writeConcern: writeConcernForSuccessfulOp}));

// PREPARE collection drop.
twoPhaseDropTest.prepareDropCollection(collName);

const writeConcernForTimedOutOp = {
    w: 'majority',
    wtimeout: 10000
};
assertTimeout(collForInserts.insert({_id: 1}, {writeConcern: writeConcernForTimedOutOp}));

// Prevent drop collection reaper from making progress after resuming oplog application.
assert.commandWorked(primary.adminCommand(
    {configureFailPoint: 'dropPendingCollectionReaperHang', mode: 'alwaysOn'}));

try {
    // Ensure that drop pending collection is not removed after resuming oplog application.
    testLog('Restarting oplog application on the secondary node.');
    twoPhaseDropTest.resumeOplogApplication(twoPhaseDropTest.replTest.getSecondary());

    // Ensure that we've hit the failpoint before moving on.
    checkLog.contains(primary, 'fail point dropPendingCollectionReaperHang enabled');

    // While the drop pending collection reaper is blocked, an operation waiting on a majority
    // write concern should time out.
    assertTimeout(collForInserts.insert({_id: 2}, {writeConcern: writeConcernForTimedOutOp}));
} finally {
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'dropPendingCollectionReaperHang', mode: 'off'}));
}

// After the reaper is unblocked, an operation waiting on a majority write concern should run
// complete successfully.
assert.writeOK(collForInserts.insert({_id: 3}, {writeConcern: writeConcernForSuccessfulOp}));
assert.eq(4, collForInserts.find().itcount());

// COMMIT collection drop.
twoPhaseDropTest.commitDropCollection(collName);

twoPhaseDropTest.stop();
}());
