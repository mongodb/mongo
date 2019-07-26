/**
 * Tests that initial sync successfully reconstructs a prepared transaction using the oplog seed
 * entry after failing the first attempt of initial sync and that we can commit the transactions
 * afterwards. During the first attempt of initial sync, there will be some oplog entries that need
 * to be applied and the lastApplied OpTime will be advanced along with the oplog application. We
 * test that the lastApplied and the localSnapshot are properly reset after the first attempt fails.
 * During the second attempt of initial sync, there will be no oplog entries that need to be applied
 * and the prepare oplog entry will be inserted as the oplog seed. We then make sure the oplog seed
 * entry is visible and the prepared transaction is properly reconstructed.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";

load("jstests/libs/check_log.js");
load("jstests/core/txns/libs/prepare_helpers.js");

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();

const config = replTest.getReplSetConfig();
// Increase the election timeout so that we do not accidentally trigger an election while the
// secondary is restarting.
config.settings = {
    "electionTimeoutMillis": 12 * 60 * 60 * 1000
};
replTest.initiate(config);

const primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

const dbName = "test";
const collName = "reconstruct_prepared_transactions_initial_sync_on_oplog_seed";

let testDB = primary.getDB(dbName);
let testColl = testDB.getCollection(collName);

const session = primary.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

assert.commandWorked(testColl.insert({_id: 1}));

jsTestLog("Restarting the secondary");

// Restart the secondary with startClean set to true so that it goes through initial sync.
replTest.stop(secondary, undefined /* signal */, {skipValidation: true});
secondary = replTest.start(
    secondary,
    {
        startClean: true,
        setParameter: {
            'numInitialSyncAttempts': 2,
            // Fail point to force the first attempt to fail and hang before starting the second
            // attempt.
            'failpoint.failAndHangInitialSync': tojson({mode: 'alwaysOn'}),
            'failpoint.initialSyncHangDuringCollectionClone': tojson(
                {mode: 'alwaysOn', data: {namespace: testColl.getFullName(), numDocsToClone: 0}}),
            'logComponentVerbosity': tojson({'replication': {'initialSync': 2}})
        }
    },
    true /* wait */);

// Wait for failpoint to be reached so we know that collection cloning is paused.
checkLog.contains(secondary, "initialSyncHangDuringCollectionClone fail point enabled");

jsTestLog("Running operations while collection cloning is paused");

// Perform writes while collection cloning is paused so that we know they must be applied during
// the first attempt of initial sync.
assert.commandWorked(testColl.insert({_id: 2}));

jsTestLog("Resuming initial sync");

// Resume initial sync.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "initialSyncHangDuringCollectionClone", mode: "off"}));

// Wait for failpoint to be reached so we know that first attempt is finishing and is about to
// fail.
checkLog.contains(secondary, "failAndHangInitialSync fail point enabled");

jsTestLog("Preparing the transaction before the second attempt of initial sync");

session.startTransaction();
assert.commandWorked(sessionColl.update({_id: 1}, {_id: 1, a: 1}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session, {w: 1});

jsTestLog("Resuming initial sync for the second attempt");
// Resume initial sync.
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "failAndHangInitialSync", mode: "off"}));

// Wait for the secondary to complete initial sync.
replTest.awaitSecondaryNodes();
PrepareHelpers.awaitMajorityCommitted(replTest, prepareTimestamp);

jsTestLog("Initial sync completed");

secondary.setSlaveOk();
const secondaryColl = secondary.getDB(dbName).getCollection(collName);

jsTestLog("Checking that the transaction is properly prepared");

// Make sure that we can't read changes to the document from the prepared transaction after
// initial sync.
assert.eq(secondaryColl.findOne({_id: 1}), {_id: 1});

jsTestLog("Committing the transaction");

assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
replTest.awaitReplication();

// Make sure that we can see the data from the committed transaction on the secondary if it was
// applied during secondary oplog application.
assert.eq(secondaryColl.findOne({_id: 1}), {_id: 1, a: 1});

replTest.stopSet();
})();
