/**
 * This test involves three nodes and runs as follows:
 *  - Node A is primary and accepts a prepared transaction.
 *  - Node B is restarted and starts initial syncing from Node A.
 *  - Node B finishes initial sync and the transaction is still prepared.
 *  - Node C is restarted and starts initial syncing from Node B.
 *  - Node C finishes initial sync and the transaction is still prepared.
 *  - Node A commits the transaction.
 *
 * This ensures that we can correctly replicate prepare oplog entries that were received
 * during initial sync.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */

(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/replsets/rslib.js");

/**
 * Restarts a secondary node so that it goes through initial sync and forces it to sync from
 * a specific sync source.
 *
 * We will confirm that the secondary properly replicated the prepare oplog entry by performing
 * an afterClusterTime read that encounters a prepare conflict.
 *
 */
function restartSecondaryAndForceSyncSource(replSet, secondary, syncSource, dbName, clusterTime) {
    // Restart secondary with startClean: true so that it goes through initial sync.
    replSet.stop(secondary,
                 // signal
                 undefined,
                 // Validation would encounter a prepare conflict on the open transaction.
                 {skipValidation: true});
    replSet.start(secondary,
                  {
                      startClean: true,
                      // Force this secondary to sync from the primary.
                      setParameter: {
                          'failpoint.forceSyncSourceCandidate':
                              tojson({mode: 'alwaysOn', data: {hostAndPort: syncSource.host}}),
                      }
                  },
                  true /* wait */);

    // Wait for the secondary to complete initial sync.
    waitForState(secondary, ReplSetTest.State.SECONDARY);
    // Allow for secondary reads.
    secondary.setSecondaryOk();
    const secondaryDB = secondary.getDB(dbName);

    // Confirm that we have a prepared transaction in progress on the secondary.
    // Use a 5 second timeout so that there is enough time for the prepared transaction to
    // release its locks and for the command to obtain those locks.
    assert.commandFailedWithCode(
        // Use afterClusterTime read to make sure that it will block on a prepare conflict.
        secondaryDB.runCommand({
            find: name,
            filter: {_id: 0},
            readConcern: {afterClusterTime: clusterTime},
            maxTimeMS: 5000
        }),
        ErrorCodes.MaxTimeMSExpired);
}

// Add secondary nodes with priority: 0 and votes: 0 so that we prevent elections while
// syncing from the primary.
const name = jsTestName();
const replSet = new ReplSetTest({
    name: name,
    nodes: [{}, {rsConfig: {priority: 0, votes: 0}}, {rsConfig: {priority: 0, votes: 0}}],
});

replSet.startSet();
replSet.initiate();
const primary = replSet.getPrimary();
const secondaries = replSet.getSecondaries();
const secondary1 = secondaries[0];
const secondary2 = secondaries[1];
const dbName = 'test';

const coll = primary.getDB(dbName).getCollection(name);
// Insert document that will be updated by a prepared transaction.
assert.commandWorked(coll.insert({_id: 0, x: 1}));

const session = primary.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(name);

jsTestLog("Preparing a transaction on the primary.");
session.startTransaction();
assert.commandWorked(sessionColl.update({_id: 0}, {x: 2}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

// Advance the clusterTime with another insert.
const clusterTimeAfterPrepare =
    assert.commandWorked(coll.runCommand("insert", {documents: [{advanceClusterTime: 1}]}))
        .operationTime;

jsTestLog("Restarting secondary1");
restartSecondaryAndForceSyncSource(replSet, secondary1, primary, dbName, clusterTimeAfterPrepare);
jsTestLog("secondary1 successfully replicated prepared transaction after initial sync");

jsTestLog("Restarting secondary2");
restartSecondaryAndForceSyncSource(
    replSet, secondary2, secondary1, dbName, clusterTimeAfterPrepare);
jsTestLog("secondary2 successfully replicated prepared transaction after initial sync");

// Commit the transaction.
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

replSet.stopSet();
})();
